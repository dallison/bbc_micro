#include "chrono_host.h"

// Host-side TZif cache and query service.
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum { DAVE_TZIF_MAGIC_SIZE = 4 };
enum { DAVE_TZIF_HEADER_SIZE = 44 };
enum { DAVE_TZIF_TTINFO_SIZE = 6 };
enum { DAVE_TZIF_LEAP_V1_SIZE = 8 };
enum { DAVE_TZIF_LEAP_V2_SIZE = 12 };
enum { DAVE_TZIF_TTIS_SIZE = 1 };

typedef struct {
  int32_t utc_offset;
  int is_dst;
  char* abbrev;
} DaveHostTzifType;

typedef struct {
  int64_t* transition_times;
  uint8_t* transition_types;
  size_t time_count;
  DaveHostTzifType* types;
  size_t type_count;
  int64_t* leap_times;
  int32_t* leap_corrections;
  size_t leap_count;
} DaveHostTzifZone;

typedef struct {
  char* name;
  char* canonical_name;
  DaveHostTzifZone zone;
  bool loaded;
} DaveHostZoneEntry;

typedef struct {
  pthread_mutex_t mutex;
  uint64_t generation;
  char* version;
  char* tzdir;
  bool utc_fallback;
  DaveHostZoneEntry* entries;
  size_t entry_count;
} DaveHostChronoCache;

static DaveHostChronoCache dave_chrono_cache = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .generation = 1,
};

static int64_t DaveHostChronoError(int host_errno) {
  switch (host_errno) {
    case ENOENT: return -DAVE_HOST_ENOENT;
    case ENOMEM: return -DAVE_HOST_ENOMEM;
    case EINVAL: return -DAVE_HOST_EINVAL;
    case ERANGE: return -DAVE_HOST_ERANGE;
    case EIO: return -DAVE_HOST_EIO;
    default: return -DAVE_HOST_EUNKNOWN;
  }
}

static int64_t DaveHostChronoNegate(int code) { return -(int64_t)code; }

static bool DaveHostChronoBounds(const unsigned char* cursor,
                                 const unsigned char* end, size_t size) {
  return cursor != NULL && end != NULL && size <= (size_t)(end - cursor);
}

static uint32_t DaveHostChronoReadBe32(const unsigned char* data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static int64_t DaveHostChronoReadBe64(const unsigned char* data) {
  uint64_t value = ((uint64_t)data[0] << 56) | ((uint64_t)data[1] << 48) |
                   ((uint64_t)data[2] << 40) | ((uint64_t)data[3] << 32) |
                   ((uint64_t)data[4] << 24) | ((uint64_t)data[5] << 16) |
                   ((uint64_t)data[6] << 8) | (uint64_t)data[7];
  return (int64_t)value;
}

static char* DaveHostChronoDuplicateString(const char* value) {
  if (value == NULL) {
    return NULL;
  }
  size_t length = strlen(value);
  char* copy = (char*)malloc(length + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, length + 1);
  return copy;
}

static int64_t DaveHostChronoCopyString(const char* source, char* buffer,
                                        size_t capacity) {
  if (buffer == NULL || capacity == 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  if (source == NULL) {
    buffer[0] = '\0';
    return 0;
  }
  size_t length = strlen(source);
  if (length + 1 > capacity) {
    return DaveHostChronoNegate(DAVE_HOST_ERANGE);
  }
  memcpy(buffer, source, length + 1);
  return 0;
}

static void DaveHostChronoFreeZone(DaveHostTzifZone* zone) {
  if (zone == NULL) {
    return;
  }
  free(zone->transition_times);
  free(zone->transition_types);
  if (zone->types != NULL) {
    for (size_t index = 0; index < zone->type_count; ++index) {
      free(zone->types[index].abbrev);
    }
  }
  free(zone->types);
  free(zone->leap_times);
  free(zone->leap_corrections);
  memset(zone, 0, sizeof(*zone));
}

static void DaveHostChronoFreeEntry(DaveHostZoneEntry* entry) {
  if (entry == NULL) {
    return;
  }
  free(entry->name);
  free(entry->canonical_name);
  DaveHostChronoFreeZone(&entry->zone);
  memset(entry, 0, sizeof(*entry));
}

static void DaveHostChronoClearCacheLocked(void) {
  if (dave_chrono_cache.entries != NULL) {
    for (size_t index = 0; index < dave_chrono_cache.entry_count; ++index) {
      DaveHostChronoFreeEntry(&dave_chrono_cache.entries[index]);
    }
  }
  free(dave_chrono_cache.entries);
  free(dave_chrono_cache.version);
  free(dave_chrono_cache.tzdir);
  dave_chrono_cache.entries = NULL;
  dave_chrono_cache.entry_count = 0;
  dave_chrono_cache.version = NULL;
  dave_chrono_cache.tzdir = NULL;
  dave_chrono_cache.utc_fallback = false;
}

static const char* DaveHostChronoDefaultTzdir(void) {
  const char* override = getenv("DAVE_TZDIR");
  if (override != NULL && override[0] != '\0') {
    return override;
  }
  const char* tzdir = getenv("TZDIR");
  if (tzdir != NULL && tzdir[0] != '\0') {
    return tzdir;
  }
  return "/usr/share/zoneinfo";
}

static char* DaveHostChronoReadVersionFile(const char* tzdir) {
  char path[PATH_MAX];
  static const char* candidates[] = {"tzdata/version", "+VERSION", "version"};
  for (size_t index = 0; index < sizeof(candidates) / sizeof(candidates[0]);
       ++index) {
    if (snprintf(path, sizeof(path), "%s/%s", tzdir, candidates[index]) >=
        (int)sizeof(path)) {
      continue;
    }
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
      continue;
    }
    char buffer[64];
    size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    if (read == 0) {
      continue;
    }
    while (read > 0 &&
           (buffer[read - 1] == '\n' || buffer[read - 1] == '\r')) {
      --read;
    }
    buffer[read] = '\0';
    return DaveHostChronoDuplicateString(buffer);
  }
  return DaveHostChronoDuplicateString("unknown");
}

static int DaveHostChronoCompareNames(const void* left, const void* right) {
  const DaveHostZoneEntry* left_entry = *(const DaveHostZoneEntry* const*)left;
  const DaveHostZoneEntry* right_entry =
      *(const DaveHostZoneEntry* const*)right;
  return strcmp(left_entry->name, right_entry->name);
}

static bool DaveHostChronoShouldSkipName(const char* name) {
  return name == NULL || name[0] == '\0' || name[0] == '.';
}

static int64_t DaveHostChronoAppendEntry(DaveHostZoneEntry** entries,
                                         size_t* capacity, size_t* count,
                                         const char* name,
                                         const char* canonical_name) {
  if (*count == *capacity) {
    size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
    DaveHostZoneEntry* resized =
        (DaveHostZoneEntry*)realloc(*entries, new_capacity * sizeof(**entries));
    if (resized == NULL) {
      return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
    }
    memset(resized + *capacity, 0,
           (new_capacity - *capacity) * sizeof(**entries));
    *entries = resized;
    *capacity = new_capacity;
  }
  DaveHostZoneEntry* entry = &(*entries)[*count];
  entry->name = DaveHostChronoDuplicateString(name);
  entry->canonical_name = DaveHostChronoDuplicateString(canonical_name);
  if (entry->name == NULL || entry->canonical_name == NULL) {
    DaveHostChronoFreeEntry(entry);
    return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
  }
  ++*count;
  return 0;
}

static int64_t DaveHostChronoScanDirectory(DaveHostZoneEntry** entries,
                                           size_t* capacity, size_t* count,
                                           const char* tzdir,
                                           const char* prefix) {
  char path[PATH_MAX];
  if (prefix == NULL || prefix[0] == '\0') {
    if (snprintf(path, sizeof(path), "%s", tzdir) >= (int)sizeof(path)) {
      return DaveHostChronoNegate(DAVE_HOST_ENAMETOOLONG);
    }
  } else if (snprintf(path, sizeof(path), "%s/%s", tzdir, prefix) >=
             (int)sizeof(path)) {
    return DaveHostChronoNegate(DAVE_HOST_ENAMETOOLONG);
  }

  DIR* directory = opendir(path);
  if (directory == NULL) {
    return DaveHostChronoError(errno);
  }

  struct dirent* entry;
  while ((entry = readdir(directory)) != NULL) {
    if (DaveHostChronoShouldSkipName(entry->d_name)) {
      continue;
    }
    if (strcmp(entry->d_name, "tzdata") == 0) {
      continue;
    }
    char child_path[PATH_MAX];
    char zone_name[DAVE_HOST_CHRONO_NAME_MAX];
    if (prefix == NULL || prefix[0] == '\0') {
      if (snprintf(child_path, sizeof(child_path), "%s/%s", tzdir,
                   entry->d_name) >= (int)sizeof(child_path) ||
          snprintf(zone_name, sizeof(zone_name), "%s", entry->d_name) >=
              (int)sizeof(zone_name)) {
        closedir(directory);
        return DaveHostChronoNegate(DAVE_HOST_ENAMETOOLONG);
      }
    } else if (snprintf(child_path, sizeof(child_path), "%s/%s/%s", tzdir,
                        prefix, entry->d_name) >= (int)sizeof(child_path) ||
               snprintf(zone_name, sizeof(zone_name), "%s/%s", prefix,
                        entry->d_name) >= (int)sizeof(zone_name)) {
      closedir(directory);
      return DaveHostChronoNegate(DAVE_HOST_ENAMETOOLONG);
    }

    struct stat status;
    if (lstat(child_path, &status) != 0) {
      continue;
    }
    if (S_ISDIR(status.st_mode)) {
      int64_t scan_result =
          DaveHostChronoScanDirectory(entries, capacity, count, tzdir, zone_name);
      if (scan_result < 0) {
        closedir(directory);
        return scan_result;
      }
      continue;
    }
    if (!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode)) {
      continue;
    }
    int64_t append_result =
        DaveHostChronoAppendEntry(entries, capacity, count, zone_name, zone_name);
    if (append_result < 0) {
      closedir(directory);
      return append_result;
    }
  }

  closedir(directory);
  return 0;
}

static int64_t DaveHostChronoEnsureIndexLocked(void) {
  if (dave_chrono_cache.entries != NULL) {
    return 0;
  }

  const char* tzdir = DaveHostChronoDefaultTzdir();
  dave_chrono_cache.tzdir = DaveHostChronoDuplicateString(tzdir);
  dave_chrono_cache.version = DaveHostChronoReadVersionFile(tzdir);
  if (dave_chrono_cache.tzdir == NULL || dave_chrono_cache.version == NULL) {
    DaveHostChronoClearCacheLocked();
    return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
  }

  DaveHostZoneEntry* entries = NULL;
  size_t capacity = 0;
  size_t count = 0;
  int64_t scan_result = DaveHostChronoScanDirectory(&entries, &capacity, &count,
                                                    tzdir, NULL);
  if (scan_result < 0) {
    if (entries != NULL) {
      for (size_t index = 0; index < count; ++index) {
        DaveHostChronoFreeEntry(&entries[index]);
      }
    }
    free(entries);
    DaveHostChronoClearCacheLocked();
    dave_chrono_cache.tzdir = DaveHostChronoDuplicateString("UTC");
    dave_chrono_cache.version = DaveHostChronoDuplicateString("UTC");
    if (dave_chrono_cache.tzdir == NULL || dave_chrono_cache.version == NULL) {
      DaveHostChronoClearCacheLocked();
      return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
    }
    dave_chrono_cache.utc_fallback = true;
    entries = NULL;
    capacity = 0;
    count = 0;
    int64_t append_result =
        DaveHostChronoAppendEntry(&entries, &capacity, &count, "UTC", "UTC");
    if (append_result < 0) {
      free(entries);
      DaveHostChronoClearCacheLocked();
      return append_result;
    }
  }

  if (count == 0) {
    free(entries);
    DaveHostChronoClearCacheLocked();
    dave_chrono_cache.tzdir = DaveHostChronoDuplicateString("UTC");
    dave_chrono_cache.version = DaveHostChronoDuplicateString("UTC");
    if (dave_chrono_cache.tzdir == NULL || dave_chrono_cache.version == NULL) {
      DaveHostChronoClearCacheLocked();
      return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
    }
    dave_chrono_cache.utc_fallback = true;
    entries = NULL;
    capacity = 0;
    count = 0;
    int64_t append_result =
        DaveHostChronoAppendEntry(&entries, &capacity, &count, "UTC", "UTC");
    if (append_result < 0) {
      free(entries);
      DaveHostChronoClearCacheLocked();
      return append_result;
    }
  }

  DaveHostZoneEntry** sort_keys =
      (DaveHostZoneEntry**)malloc(count * sizeof(DaveHostZoneEntry*));
  if (sort_keys == NULL) {
    for (size_t index = 0; index < count; ++index) {
      DaveHostChronoFreeEntry(&entries[index]);
    }
    free(entries);
    DaveHostChronoClearCacheLocked();
    return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
  }
  for (size_t index = 0; index < count; ++index) {
    sort_keys[index] = &entries[index];
  }
  qsort(sort_keys, count, sizeof(DaveHostZoneEntry*), DaveHostChronoCompareNames);

  DaveHostZoneEntry* sorted =
      (DaveHostZoneEntry*)calloc(count, sizeof(DaveHostZoneEntry));
  if (sorted == NULL) {
    free(sort_keys);
    for (size_t index = 0; index < count; ++index) {
      DaveHostChronoFreeEntry(&entries[index]);
    }
    free(entries);
    DaveHostChronoClearCacheLocked();
    return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
  }
  for (size_t index = 0; index < count; ++index) {
    sorted[index] = *sort_keys[index];
  }
  free(sort_keys);
  free(entries);

  dave_chrono_cache.entries = sorted;
  dave_chrono_cache.entry_count = count;
  return 0;
}

static int64_t DaveHostChronoParseBlock(const unsigned char* data,
                                        const unsigned char* end, bool use64,
                                        DaveHostTzifZone* zone) {
  if (!DaveHostChronoBounds(data, end, DAVE_TZIF_HEADER_SIZE)) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  if (memcmp(data, "TZif", DAVE_TZIF_MAGIC_SIZE) != 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }

  uint32_t ttisgmt_count = DaveHostChronoReadBe32(data + 20);
  uint32_t ttisut_count = DaveHostChronoReadBe32(data + 24);
  uint32_t time_count = DaveHostChronoReadBe32(data + 28);
  uint32_t type_count = DaveHostChronoReadBe32(data + 32);
  uint32_t char_count = DaveHostChronoReadBe32(data + 36);
  uint32_t leap_count = DaveHostChronoReadBe32(data + 40);

  const unsigned char* cursor = data + DAVE_TZIF_HEADER_SIZE;
  size_t time_size = use64 ? 8 : 4;
  size_t leap_size = use64 ? DAVE_TZIF_LEAP_V2_SIZE : DAVE_TZIF_LEAP_V1_SIZE;

  if (time_count > INT32_MAX || type_count == 0 || type_count > INT32_MAX ||
      char_count == 0 || char_count > INT32_MAX || leap_count > INT32_MAX ||
      ttisut_count > INT32_MAX || ttisgmt_count > INT32_MAX) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }

  size_t transition_bytes = (size_t)time_count * time_size;
  size_t type_index_bytes = (size_t)time_count * DAVE_TZIF_TTIS_SIZE;
  size_t ttinfo_bytes = (size_t)type_count * DAVE_TZIF_TTINFO_SIZE;
  size_t leap_bytes = (size_t)leap_count * leap_size;
  size_t ttisut_bytes = (size_t)ttisut_count * DAVE_TZIF_TTIS_SIZE;
  size_t ttisgmt_bytes = (size_t)ttisgmt_count * DAVE_TZIF_TTIS_SIZE;
  if (!DaveHostChronoBounds(cursor, end, transition_bytes + type_index_bytes +
                                              ttinfo_bytes + char_count +
                                              leap_bytes + ttisut_bytes +
                                              ttisgmt_bytes)) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }

  int64_t* transition_times = NULL;
  uint8_t* transition_types = NULL;
  DaveHostTzifType* types = NULL;
  int64_t* leap_times = NULL;
  int32_t* leap_corrections = NULL;

  if (time_count > 0) {
    transition_times = (int64_t*)calloc(time_count, sizeof(int64_t));
    transition_types = (uint8_t*)malloc(time_count);
    if (transition_times == NULL || transition_types == NULL) {
      free(transition_times);
      free(transition_types);
      return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
    }
    for (uint32_t index = 0; index < time_count; ++index) {
      if (use64) {
        transition_times[index] = DaveHostChronoReadBe64(cursor);
        cursor += 8;
      } else {
        transition_times[index] =
            (int64_t)(int32_t)DaveHostChronoReadBe32(cursor);
        cursor += 4;
      }
    }
    memcpy(transition_types, cursor, time_count);
    cursor += time_count;
    for (uint32_t index = 0; index < time_count; ++index) {
      if (transition_types[index] >= type_count) {
        free(transition_times);
        free(transition_types);
        return DaveHostChronoNegate(DAVE_HOST_EINVAL);
      }
    }
  }

  types = (DaveHostTzifType*)calloc(type_count, sizeof(DaveHostTzifType));
  if (types == NULL) {
    free(transition_times);
    free(transition_types);
    return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
  }
  const unsigned char* ttinfo_base = cursor;
  for (uint32_t index = 0; index < type_count; ++index) {
    types[index].utc_offset = (int32_t)DaveHostChronoReadBe32(cursor);
    types[index].is_dst = cursor[4] != 0;
    uint8_t abbr_index = cursor[5];
    cursor += DAVE_TZIF_TTINFO_SIZE;
    if (abbr_index >= char_count) {
      free(transition_times);
      free(transition_types);
      free(types);
      return DaveHostChronoNegate(DAVE_HOST_EINVAL);
    }
    (void)abbr_index;
    (void)ttinfo_base;
  }

  const unsigned char* abbrev_base = cursor;
  if (memchr(abbrev_base, '\0', char_count) == NULL) {
    free(transition_times);
    free(transition_types);
    free(types);
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  cursor += char_count;
  for (uint32_t index = 0; index < type_count; ++index) {
    uint8_t abbr_index = ttinfo_base[index * DAVE_TZIF_TTINFO_SIZE + 5];
    types[index].abbrev =
        DaveHostChronoDuplicateString((const char*)(abbrev_base + abbr_index));
    if (types[index].abbrev == NULL) {
      for (uint32_t cleanup = 0; cleanup < index; ++cleanup) {
        free(types[cleanup].abbrev);
      }
      free(transition_times);
      free(transition_types);
      free(types);
      return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
    }
  }

  if (leap_count > 0) {
    leap_times = (int64_t*)calloc(leap_count, sizeof(int64_t));
    leap_corrections = (int32_t*)calloc(leap_count, sizeof(int32_t));
    if (leap_times == NULL || leap_corrections == NULL) {
      free(leap_times);
      free(leap_corrections);
      for (uint32_t cleanup = 0; cleanup < type_count; ++cleanup) {
        free(types[cleanup].abbrev);
      }
      free(transition_times);
      free(transition_types);
      free(types);
      return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
    }
    for (uint32_t index = 0; index < leap_count; ++index) {
      if (use64) {
        leap_times[index] = DaveHostChronoReadBe64(cursor);
        leap_corrections[index] = (int32_t)DaveHostChronoReadBe32(cursor + 8);
        cursor += DAVE_TZIF_LEAP_V2_SIZE;
      } else {
        leap_times[index] = (int64_t)(int32_t)DaveHostChronoReadBe32(cursor);
        leap_corrections[index] = (int32_t)DaveHostChronoReadBe32(cursor + 4);
        cursor += DAVE_TZIF_LEAP_V1_SIZE;
      }
    }
  }

  if (!DaveHostChronoBounds(cursor, end, ttisut_bytes + ttisgmt_bytes)) {
    DaveHostChronoFreeZone(zone);
    free(transition_times);
    free(transition_types);
    for (uint32_t cleanup = 0; cleanup < type_count; ++cleanup) {
      free(types[cleanup].abbrev);
    }
    free(types);
    free(leap_times);
    free(leap_corrections);
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }

  zone->transition_times = transition_times;
  zone->transition_types = transition_types;
  zone->time_count = time_count;
  zone->types = types;
  zone->type_count = type_count;
  zone->leap_times = leap_times;
  zone->leap_corrections = leap_corrections;
  zone->leap_count = leap_count;
  return 0;
}

static size_t DaveHostChronoV1BlockSize(const unsigned char* data,
                                        const unsigned char* end) {
  if (!DaveHostChronoBounds(data, end, DAVE_TZIF_HEADER_SIZE)) {
    return 0;
  }
  uint32_t ttisgmt_count = DaveHostChronoReadBe32(data + 20);
  uint32_t ttisut_count = DaveHostChronoReadBe32(data + 24);
  uint32_t time_count = DaveHostChronoReadBe32(data + 28);
  uint32_t type_count = DaveHostChronoReadBe32(data + 32);
  uint32_t char_count = DaveHostChronoReadBe32(data + 36);
  uint32_t leap_count = DaveHostChronoReadBe32(data + 40);
  return DAVE_TZIF_HEADER_SIZE + (size_t)time_count * 4 + (size_t)time_count +
         (size_t)type_count * DAVE_TZIF_TTINFO_SIZE + (size_t)char_count +
         (size_t)leap_count * DAVE_TZIF_LEAP_V1_SIZE + (size_t)ttisgmt_count +
         (size_t)ttisut_count;
}

static const unsigned char* DaveHostChronoFindV2Marker(const unsigned char* data,
                                                     const unsigned char* end) {
  size_t v1_size = DaveHostChronoV1BlockSize(data, end);
  if (v1_size == 0 || !DaveHostChronoBounds(data, end, v1_size + 3)) {
    return NULL;
  }
  const unsigned char* marker = data + v1_size;
  if (marker[0] == '\n' && (marker[1] == '2' || marker[1] == '3') &&
      marker[2] == '\n') {
    return marker;
  }
  return NULL;
}

static int64_t DaveHostChronoParseTzif(const unsigned char* data, size_t size,
                                       DaveHostTzifZone* zone) {
  if (data == NULL || size < DAVE_TZIF_HEADER_SIZE || zone == NULL) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  memset(zone, 0, sizeof(*zone));

  const unsigned char* end = data + size;
  char version = (char)data[4];
  const unsigned char* v2_marker = DaveHostChronoFindV2Marker(data, end);
  if (version == '2' || version == '3' || v2_marker != NULL) {
    if (v2_marker == NULL) {
      return DaveHostChronoNegate(DAVE_HOST_EINVAL);
    }
    const unsigned char* cursor = v2_marker + 3;
    return DaveHostChronoParseBlock(cursor, end, true, zone);
  }
  return DaveHostChronoParseBlock(data, end, false, zone);
}

static int64_t DaveHostChronoReadZoneFile(const char* path,
                                          DaveHostTzifZone* zone) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    return DaveHostChronoError(errno);
  }
  if (fseeko(file, 0, SEEK_END) != 0) {
    fclose(file);
    return DaveHostChronoError(errno);
  }
  off_t size = ftello(file);
  if (size < 0) {
    fclose(file);
    return DaveHostChronoError(errno);
  }
  if ((size_t)size > 16 * 1024 * 1024) {
    fclose(file);
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  if (fseeko(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return DaveHostChronoError(errno);
  }
  unsigned char* data = (unsigned char*)malloc((size_t)size);
  if (data == NULL) {
    fclose(file);
    return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
  }
  size_t read = fread(data, 1, (size_t)size, file);
  fclose(file);
  if (read != (size_t)size) {
    free(data);
    return DaveHostChronoNegate(DAVE_HOST_EIO);
  }
  int64_t parse_result = DaveHostChronoParseTzif(data, read, zone);
  free(data);
  return parse_result;
}

static int64_t DaveHostChronoResolveCanonicalName(const char* name,
                                                  char* buffer, size_t capacity) {
  if (name == NULL || buffer == NULL || capacity == 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  char path[PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/%s", dave_chrono_cache.tzdir, name) >=
      (int)sizeof(path)) {
    return DaveHostChronoNegate(DAVE_HOST_ENAMETOOLONG);
  }

  char target[PATH_MAX];
  ssize_t link_length = readlink(path, target, sizeof(target) - 1);
  if (link_length < 0) {
    return DaveHostChronoCopyString(name, buffer, capacity);
  }
  target[link_length] = '\0';
  const char* zone_root = dave_chrono_cache.tzdir;
  size_t root_length = strlen(zone_root);
  if (link_length > (ssize_t)root_length && strncmp(target, zone_root, root_length) == 0 &&
      target[root_length] == '/') {
    return DaveHostChronoCopyString(target + root_length + 1, buffer, capacity);
  }
  const char* basename = strrchr(target, '/');
  return DaveHostChronoCopyString(basename != NULL ? basename + 1 : target,
                                  buffer, capacity);
}

static ssize_t DaveHostChronoFindZoneIndex(const char* name) {
  if (name == NULL) {
    return -1;
  }
  for (size_t index = 0; index < dave_chrono_cache.entry_count; ++index) {
    if (strcmp(dave_chrono_cache.entries[index].name, name) == 0) {
      return (ssize_t)index;
    }
  }
  return -1;
}

static int64_t DaveHostChronoEnsureZoneLoadedLocked(size_t index) {
  if (index >= dave_chrono_cache.entry_count) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  DaveHostZoneEntry* entry = &dave_chrono_cache.entries[index];
  if (entry->loaded) {
    return 0;
  }
  if (dave_chrono_cache.utc_fallback &&
      strcmp(entry->canonical_name, "UTC") == 0) {
    entry->zone.type_count = 1;
    entry->zone.types =
        (DaveHostTzifType*)calloc(1, sizeof(DaveHostTzifType));
    if (entry->zone.types == NULL) {
      return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
    }
    entry->zone.types[0].utc_offset = 0;
    entry->zone.types[0].is_dst = 0;
    entry->zone.types[0].abbrev = DaveHostChronoDuplicateString("UTC");
    if (entry->zone.types[0].abbrev == NULL) {
      DaveHostChronoFreeZone(&entry->zone);
      return DaveHostChronoNegate(DAVE_HOST_ENOMEM);
    }
    entry->loaded = true;
    return 0;
  }

  char path[PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/%s", dave_chrono_cache.tzdir,
               entry->name) >= (int)sizeof(path)) {
    return DaveHostChronoNegate(DAVE_HOST_ENAMETOOLONG);
  }
  int64_t load_result = DaveHostChronoReadZoneFile(path, &entry->zone);
  if (load_result < 0) {
    return load_result;
  }
  entry->loaded = true;
  return 0;
}

static size_t DaveHostChronoTypeIndexForTime(const DaveHostTzifZone* zone,
                                             int64_t sys_seconds) {
  if (zone->time_count == 0) {
    return 0;
  }
  size_t index = zone->time_count;
  while (index > 0 && zone->transition_times[index - 1] > sys_seconds) {
    --index;
  }
  if (index == 0) {
    return 0;
  }
  return zone->transition_types[index - 1];
}

static void DaveHostChronoFillSysPeriod(const DaveHostTzifZone* zone,
                                        int64_t sys_seconds,
                                        DaveHostChronoSysInfoWire* result) {
  size_t type_index = DaveHostChronoTypeIndexForTime(zone, sys_seconds);
  result->offset_seconds = zone->types[type_index].utc_offset;
  result->flags = zone->types[type_index].is_dst ? 1u : 0u;
  result->reserved = 0;
  if (zone->time_count == 0) {
    result->begin_seconds = INT64_MIN;
    result->end_seconds = INT64_MAX;
    return;
  }
  size_t transition_index = zone->time_count;
  while (transition_index > 0 &&
         zone->transition_times[transition_index - 1] > sys_seconds) {
    --transition_index;
  }
  if (transition_index == 0) {
    result->begin_seconds = INT64_MIN;
    result->end_seconds = zone->transition_times[0];
    return;
  }
  result->begin_seconds = zone->transition_times[transition_index - 1];
  if (transition_index < zone->time_count) {
    result->end_seconds = zone->transition_times[transition_index];
  } else {
    result->end_seconds = INT64_MAX;
  }
}

static int64_t DaveHostChronoQueryZoneByName(const char* zone_name,
                                             DaveHostZoneEntry** entry_out) {
  if (zone_name == NULL || entry_out == NULL) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  int64_t ensure_result = DaveHostChronoEnsureIndexLocked();
  if (ensure_result < 0) {
    return ensure_result;
  }
  if (dave_chrono_cache.utc_fallback && strcmp(zone_name, "UTC") != 0) {
    return DaveHostChronoNegate(DAVE_HOST_ENOENT);
  }
  char canonical[DAVE_HOST_CHRONO_NAME_MAX];
  int64_t canonical_result =
      DaveHostChronoResolveCanonicalName(zone_name, canonical, sizeof(canonical));
  if (canonical_result < 0) {
    return canonical_result;
  }
  ssize_t index = DaveHostChronoFindZoneIndex(canonical);
  if (index < 0) {
    index = DaveHostChronoFindZoneIndex(zone_name);
  }
  if (index < 0) {
    return DaveHostChronoNegate(DAVE_HOST_ENOENT);
  }
  int64_t load_result = DaveHostChronoEnsureZoneLoadedLocked((size_t)index);
  if (load_result < 0) {
    return load_result;
  }
  *entry_out = &dave_chrono_cache.entries[(size_t)index];
  return 0;
}

static int64_t DaveHostChronoDetectCurrentZone(char* buffer, size_t capacity) {
  const char* tz = getenv("TZ");
  if (tz != NULL && tz[0] != '\0') {
    if (tz[0] == ':') {
      ++tz;
    }
    const char* end = strchr(tz, ':');
    size_t length = end != NULL ? (size_t)(end - tz) : strlen(tz);
    if (length + 1 > capacity) {
      return DaveHostChronoNegate(DAVE_HOST_ERANGE);
    }
    memcpy(buffer, tz, length);
    buffer[length] = '\0';
    return 0;
  }

  char link_target[PATH_MAX];
  ssize_t link_length =
      readlink("/etc/localtime", link_target, sizeof(link_target) - 1);
  if (link_length > 0) {
    link_target[link_length] = '\0';
    const char* marker = strstr(link_target, "zoneinfo/");
    if (marker != NULL) {
      return DaveHostChronoCopyString(marker + strlen("zoneinfo/"), buffer,
                                      capacity);
    }
    const char* basename = strrchr(link_target, '/');
    if (basename != NULL) {
      return DaveHostChronoCopyString(basename + 1, buffer, capacity);
    }
  }
  return DaveHostChronoCopyString("UTC", buffer, capacity);
}

int64_t DaveHostChronoTzdbVersion(char* buffer, size_t capacity) {
  if (buffer == NULL || capacity == 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  int64_t ensure_result = DaveHostChronoEnsureIndexLocked();
  if (ensure_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return ensure_result;
  }
  int64_t copy_result =
      DaveHostChronoCopyString(dave_chrono_cache.version, buffer, capacity);
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return copy_result;
}

int64_t DaveHostChronoGeneration(uint64_t* generation) {
  if (generation == NULL) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  *generation = dave_chrono_cache.generation;
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return 0;
}

int64_t DaveHostChronoReload(uint64_t* generation) {
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  DaveHostChronoClearCacheLocked();
  ++dave_chrono_cache.generation;
  if (generation != NULL) {
    *generation = dave_chrono_cache.generation;
  }
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return 0;
}

int64_t DaveHostChronoCurrentZone(char* buffer, size_t capacity) {
  if (buffer == NULL || capacity == 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  int64_t ensure_result = DaveHostChronoEnsureIndexLocked();
  if (ensure_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return ensure_result;
  }
  char requested[DAVE_HOST_CHRONO_NAME_MAX];
  int64_t current_result =
      DaveHostChronoDetectCurrentZone(requested, sizeof(requested));
  if (current_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return current_result;
  }
  char canonical[DAVE_HOST_CHRONO_NAME_MAX];
  int64_t canonical_result =
      DaveHostChronoResolveCanonicalName(requested, canonical, sizeof(canonical));
  if (canonical_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return canonical_result;
  }
  if (DaveHostChronoFindZoneIndex(canonical) < 0 &&
      DaveHostChronoFindZoneIndex(requested) < 0) {
    if (dave_chrono_cache.utc_fallback) {
      canonical_result = DaveHostChronoCopyString("UTC", canonical, sizeof(canonical));
    } else {
      pthread_mutex_unlock(&dave_chrono_cache.mutex);
      return DaveHostChronoNegate(DAVE_HOST_ENOENT);
    }
  }
  int64_t copy_result = DaveHostChronoCopyString(
      DaveHostChronoFindZoneIndex(canonical) >= 0 ? canonical : requested, buffer,
      capacity);
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return copy_result;
}

int64_t DaveHostChronoZoneCount(uint32_t* count) {
  if (count == NULL) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  int64_t ensure_result = DaveHostChronoEnsureIndexLocked();
  if (ensure_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return ensure_result;
  }
  *count = (uint32_t)dave_chrono_cache.entry_count;
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return 0;
}

int64_t DaveHostChronoZoneName(uint32_t index, char* buffer, size_t capacity) {
  if (buffer == NULL || capacity == 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  int64_t ensure_result = DaveHostChronoEnsureIndexLocked();
  if (ensure_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return ensure_result;
  }
  if ((size_t)index >= dave_chrono_cache.entry_count) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  int64_t copy_result = DaveHostChronoCopyString(
      dave_chrono_cache.entries[index].name, buffer, capacity);
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return copy_result;
}

int64_t DaveHostChronoLocateZone(const char* name, char* buffer, size_t capacity,
                                 uint32_t* index) {
  if (name == NULL || buffer == NULL || capacity == 0 || index == NULL) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  int64_t ensure_result = DaveHostChronoEnsureIndexLocked();
  if (ensure_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return ensure_result;
  }
  if (dave_chrono_cache.utc_fallback && strcmp(name, "UTC") != 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return DaveHostChronoNegate(DAVE_HOST_ENOENT);
  }
  char canonical[DAVE_HOST_CHRONO_NAME_MAX];
  int64_t canonical_result =
      DaveHostChronoResolveCanonicalName(name, canonical, sizeof(canonical));
  if (canonical_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return canonical_result;
  }
  ssize_t found = DaveHostChronoFindZoneIndex(canonical);
  if (found < 0) {
    found = DaveHostChronoFindZoneIndex(name);
  }
  if (found < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return DaveHostChronoNegate(DAVE_HOST_ENOENT);
  }
  int64_t copy_result = DaveHostChronoCopyString(
      dave_chrono_cache.entries[(size_t)found].canonical_name, buffer, capacity);
  if (copy_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return copy_result;
  }
  *index = (uint32_t)found;
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return 0;
}

int64_t DaveHostChronoSysInfo(const char* zone_name, int64_t sys_seconds,
                              DaveHostChronoSysInfoWire* result, char* abbrev_buffer,
                              size_t abbrev_capacity) {
  if (result == NULL || abbrev_buffer == NULL || abbrev_capacity == 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  DaveHostZoneEntry* entry = NULL;
  int64_t query_result = DaveHostChronoQueryZoneByName(zone_name, &entry);
  if (query_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return query_result;
  }
  DaveHostChronoFillSysPeriod(&entry->zone, sys_seconds, result);
  size_t type_index = DaveHostChronoTypeIndexForTime(&entry->zone, sys_seconds);
  int64_t copy_result = DaveHostChronoCopyString(
      entry->zone.types[type_index].abbrev, abbrev_buffer, abbrev_capacity);
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return copy_result;
}

int64_t DaveHostChronoSysInfoRequest(const char* zone_name,
                                     const DaveHostChronoSysInfoRequestWire* request) {
  if (request == NULL || request->abbrev_capacity == 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  int64_t* seconds = (int64_t*)(uintptr_t)request->seconds_address;
  DaveHostChronoSysInfoWire* result =
      (DaveHostChronoSysInfoWire*)(uintptr_t)request->result_address;
  char* abbrev = (char*)(uintptr_t)request->abbrev_address;
  if (seconds == NULL || result == NULL || abbrev == NULL) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  return DaveHostChronoSysInfo(zone_name, *seconds, result, abbrev,
                               request->abbrev_capacity);
}

int64_t DaveHostChronoLocalInfo(const char* zone_name, int64_t local_seconds,
                                DaveHostChronoLocalInfoWire* result,
                                char* first_abbrev_buffer,
                                size_t first_abbrev_capacity,
                                char* second_abbrev_buffer,
                                size_t second_abbrev_capacity) {
  if (result == NULL || first_abbrev_buffer == NULL ||
      first_abbrev_capacity == 0 || second_abbrev_buffer == NULL ||
      second_abbrev_capacity == 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  memset(result, 0, sizeof(*result));
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  DaveHostZoneEntry* entry = NULL;
  int64_t query_result = DaveHostChronoQueryZoneByName(zone_name, &entry);
  if (query_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return query_result;
  }

  const DaveHostTzifZone* zone = &entry->zone;
  int matches = 0;
  DaveHostChronoSysInfoWire match_info[2];
  const char* match_abbrev[2] = {NULL, NULL};

  size_t period_count = zone->time_count == 0 ? 1 : zone->time_count + 1;
  for (size_t period = 0; period < period_count; ++period) {
    int64_t begin_sys;
    int64_t end_sys;
    size_t type_index;
    if (zone->time_count == 0) {
      begin_sys = INT64_MIN;
      end_sys = INT64_MAX;
      type_index = 0;
    } else if (period == 0) {
      begin_sys = INT64_MIN;
      end_sys = zone->transition_times[0];
      type_index = 0;
    } else {
      begin_sys = zone->transition_times[period - 1];
      end_sys = period < zone->time_count ? zone->transition_times[period]
                                          : INT64_MAX;
      type_index = zone->transition_types[period - 1];
    }
    int32_t offset = zone->types[type_index].utc_offset;
    int64_t begin_local = begin_sys + offset;
    int64_t end_local = end_sys + offset;
    if (local_seconds < begin_local || local_seconds >= end_local) {
      continue;
    }
    if (matches < 2) {
      match_info[matches].begin_seconds = begin_sys;
      match_info[matches].end_seconds = end_sys;
      match_info[matches].offset_seconds = offset;
      match_info[matches].flags = zone->types[type_index].is_dst ? 1u : 0u;
      match_info[matches].reserved = 0;
      match_abbrev[matches] = zone->types[type_index].abbrev;
      ++matches;
    }
  }

  if (matches == 0) {
    result->result = DAVE_HOST_CHRONO_LOCAL_NONEXISTENT;
  } else if (matches == 1) {
    result->result = DAVE_HOST_CHRONO_LOCAL_UNIQUE;
    result->first = match_info[0];
  } else {
    result->result = DAVE_HOST_CHRONO_LOCAL_AMBIGUOUS;
    result->first = match_info[0];
    result->second = match_info[1];
  }

  int64_t first_copy = DaveHostChronoCopyString(
      match_abbrev[0] != NULL ? match_abbrev[0] : "", first_abbrev_buffer,
      first_abbrev_capacity);
  if (first_copy < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return first_copy;
  }
  int64_t second_copy = DaveHostChronoCopyString(
      match_abbrev[1] != NULL ? match_abbrev[1] : "", second_abbrev_buffer,
      second_abbrev_capacity);
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return second_copy;
}

int64_t DaveHostChronoLocalInfoRequest(
    const char* zone_name, const DaveHostChronoLocalInfoRequestWire* request) {
  if (request == NULL || request->abbrev_capacity == 0) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  int64_t* seconds = (int64_t*)(uintptr_t)request->seconds_address;
  DaveHostChronoLocalInfoWire* result =
      (DaveHostChronoLocalInfoWire*)(uintptr_t)request->result_address;
  char* abbrev = (char*)(uintptr_t)request->abbrev_address;
  if (seconds == NULL || result == NULL || abbrev == NULL) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  return DaveHostChronoLocalInfo(
      zone_name, *seconds, result, abbrev, request->abbrev_capacity,
      abbrev + request->abbrev_capacity, request->abbrev_capacity);
}

static ssize_t DaveHostChronoFindLeapZoneIndexLocked(void) {
  static const char* candidates[] = {"right/UTC", "LeapZone", "UTC"};
  for (size_t candidate = 0;
       candidate < sizeof(candidates) / sizeof(candidates[0]); ++candidate) {
    ssize_t index = DaveHostChronoFindZoneIndex(candidates[candidate]);
    if (index < 0) {
      continue;
    }
    if (DaveHostChronoEnsureZoneLoadedLocked((size_t)index) < 0) {
      continue;
    }
    if (dave_chrono_cache.entries[(size_t)index].zone.leap_count > 0) {
      return index;
    }
  }
  return -1;
}

int64_t DaveHostChronoLeapCount(uint32_t* count) {
  if (count == NULL) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  *count = 0;
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  int64_t ensure_result = DaveHostChronoEnsureIndexLocked();
  if (ensure_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return ensure_result;
  }
  ssize_t index = DaveHostChronoFindLeapZoneIndexLocked();
  if (index < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return 0;
  }
  *count = (uint32_t)dave_chrono_cache.entries[(size_t)index].zone.leap_count;
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return 0;
}

int64_t DaveHostChronoLeapInfo(uint32_t index, DaveHostChronoLeapSecond* result) {
  if (result == NULL) {
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  pthread_mutex_lock(&dave_chrono_cache.mutex);
  int64_t ensure_result = DaveHostChronoEnsureIndexLocked();
  if (ensure_result < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return ensure_result;
  }
  ssize_t zone_index = DaveHostChronoFindLeapZoneIndexLocked();
  if (zone_index < 0) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  const DaveHostTzifZone* zone =
      &dave_chrono_cache.entries[(size_t)zone_index].zone;
  if ((size_t)index >= zone->leap_count) {
    pthread_mutex_unlock(&dave_chrono_cache.mutex);
    return DaveHostChronoNegate(DAVE_HOST_EINVAL);
  }
  result->date_seconds = zone->leap_times[index];
  result->correction_seconds = zone->leap_corrections[index];
  result->reserved = 0;
  pthread_mutex_unlock(&dave_chrono_cache.mutex);
  return 0;
}
