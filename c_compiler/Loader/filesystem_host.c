#include "filesystem_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static int DaveHostFilesystemTranslateError(int value) {
  switch (value) {
    case ENOENT: return DAVE_HOST_ENOENT;
    case ENOMEM: return DAVE_HOST_ENOMEM;
    case EACCES: return DAVE_HOST_EACCES;
    case ENODEV: return DAVE_HOST_ENODEV;
    case EMFILE: return DAVE_HOST_EMFILE;
    case EBUSY: return DAVE_HOST_EBUSY;
    case EINVAL: return DAVE_HOST_EINVAL;
    case ENOSPC: return DAVE_HOST_ENOSPC;
    case EEXIST: return DAVE_HOST_EEXIST;
    case EAGAIN: return DAVE_HOST_EAGAIN;
    case EIO: return DAVE_HOST_EIO;
    case EINTR: return DAVE_HOST_EINTR;
    case ENOSYS: return DAVE_HOST_ENOSYS;
    case ESPIPE: return DAVE_HOST_ESPIPE;
    case ERANGE: return DAVE_HOST_ERANGE;
    case EBADF: return DAVE_HOST_EBADF;
    case ENOEXEC: return DAVE_HOST_ENOEXEC;
    case EPERM: return DAVE_HOST_EPERM;
    case ESRCH: return DAVE_HOST_ESRCH;
    case EDEADLK: return DAVE_HOST_EDEADLK;
    case ENOTDIR: return DAVE_HOST_ENOTDIR;
    case EISDIR: return DAVE_HOST_EISDIR;
    case ENAMETOOLONG: return DAVE_HOST_ENAMETOOLONG;
    case ENOTEMPTY: return DAVE_HOST_ENOTEMPTY;
    case ELOOP: return DAVE_HOST_ELOOP;
    case EROFS: return DAVE_HOST_EROFS;
    case EXDEV: return DAVE_HOST_EXDEV;
    case EFBIG: return DAVE_HOST_EFBIG;
    case ENFILE: return DAVE_HOST_ENFILE;
#if defined(ENOTSUP)
    case ENOTSUP: return DAVE_HOST_ENOTSUP;
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
    case EOPNOTSUPP: return DAVE_HOST_ENOTSUP;
#endif
    default: return DAVE_HOST_EUNKNOWN;
  }
}

static int64_t DaveHostFilesystemError(void) {
  return -(int64_t)DaveHostFilesystemTranslateError(
      errno != 0 ? errno : EIO);
}

static int64_t DaveHostFilesystemTimespecNanoseconds(struct timespec value) {
  return (int64_t)value.tv_sec * 1000000000LL + (int64_t)value.tv_nsec;
}

static void DaveHostFilesystemCopyStatus(
    const struct stat* value, DaveHostFilesystemStat* result) {
  result->device = (uint64_t)value->st_dev;
  result->inode = (uint64_t)value->st_ino;
  result->size = value->st_size < 0 ? 0 : (uint64_t)value->st_size;
  result->hard_link_count = (uint64_t)value->st_nlink;
#if defined(__APPLE__)
  result->access_time_ns =
      DaveHostFilesystemTimespecNanoseconds(value->st_atimespec);
  result->modification_time_ns =
      DaveHostFilesystemTimespecNanoseconds(value->st_mtimespec);
  result->status_change_time_ns =
      DaveHostFilesystemTimespecNanoseconds(value->st_ctimespec);
#else
  result->access_time_ns =
      DaveHostFilesystemTimespecNanoseconds(value->st_atim);
  result->modification_time_ns =
      DaveHostFilesystemTimespecNanoseconds(value->st_mtim);
  result->status_change_time_ns =
      DaveHostFilesystemTimespecNanoseconds(value->st_ctim);
#endif
  result->mode = (uint32_t)value->st_mode;
  result->reserved = 0;
}

int64_t DaveHostFilesystemGetStatus(const char* path, int follow,
                                    DaveHostFilesystemStat* result) {
  if (path == NULL || result == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  struct stat value;
  if ((follow ? stat(path, &value) : lstat(path, &value)) != 0) {
    return DaveHostFilesystemError();
  }
  DaveHostFilesystemCopyStatus(&value, result);
  return 0;
}

int64_t DaveHostFilesystemGetDescriptorStatus(
    int fd, DaveHostFilesystemStat* result) {
  if (result == NULL) return -DAVE_HOST_EINVAL;
  struct stat value;
  if (fstat(fd, &value) != 0) return DaveHostFilesystemError();
  DaveHostFilesystemCopyStatus(&value, result);
  return 0;
}

static pthread_mutex_t dave_directory_mutex = PTHREAD_MUTEX_INITIALIZER;
static DIR** dave_directories;
static size_t dave_directory_capacity;

int64_t DaveHostFilesystemOpenDirectory(const char* path) {
  if (path == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  DIR* directory = opendir(path);
  if (directory == NULL) {
    return DaveHostFilesystemError();
  }

  pthread_mutex_lock(&dave_directory_mutex);
  size_t slot = 0;
  while (slot < dave_directory_capacity && dave_directories[slot] != NULL) {
    ++slot;
  }
  if (slot == dave_directory_capacity) {
    size_t new_capacity =
        dave_directory_capacity == 0 ? 16 : dave_directory_capacity * 2;
    DIR** resized =
        (DIR**)realloc(dave_directories, new_capacity * sizeof(DIR*));
    if (resized == NULL) {
      pthread_mutex_unlock(&dave_directory_mutex);
      closedir(directory);
      return -DAVE_HOST_ENOMEM;
    }
    memset(resized + dave_directory_capacity, 0,
           (new_capacity - dave_directory_capacity) * sizeof(DIR*));
    dave_directories = resized;
    dave_directory_capacity = new_capacity;
  }
  dave_directories[slot] = directory;
  pthread_mutex_unlock(&dave_directory_mutex);
  return (int64_t)(slot + 1);
}

int64_t DaveHostFilesystemReadDirectory(
    int handle, DaveHostFilesystemDirectoryEntry* result) {
  if (handle <= 0 || result == NULL) {
    return -DAVE_HOST_EINVAL;
  }

  pthread_mutex_lock(&dave_directory_mutex);
  size_t slot = (size_t)(handle - 1);
  if (slot >= dave_directory_capacity || dave_directories[slot] == NULL) {
    pthread_mutex_unlock(&dave_directory_mutex);
    return -DAVE_HOST_EBADF;
  }
  DIR* directory = dave_directories[slot];
  struct dirent* entry;
  do {
    errno = 0;
    entry = readdir(directory);
  } while (entry != NULL &&
           (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0));
  if (entry == NULL) {
    int saved_errno = errno;
    pthread_mutex_unlock(&dave_directory_mutex);
    return saved_errno == 0
               ? 0
               : -(int64_t)DaveHostFilesystemTranslateError(saved_errno);
  }
  size_t length = strlen(entry->d_name);
  if (length >= sizeof(result->name)) {
    pthread_mutex_unlock(&dave_directory_mutex);
    return -DAVE_HOST_ENAMETOOLONG;
  }
  result->type = (uint32_t)entry->d_type;
  result->reserved = 0;
  memcpy(result->name, entry->d_name, length + 1);
  pthread_mutex_unlock(&dave_directory_mutex);
  return 1;
}

int64_t DaveHostFilesystemCloseDirectory(int handle) {
  if (handle <= 0) {
    return -DAVE_HOST_EINVAL;
  }
  pthread_mutex_lock(&dave_directory_mutex);
  size_t slot = (size_t)(handle - 1);
  if (slot >= dave_directory_capacity || dave_directories[slot] == NULL) {
    pthread_mutex_unlock(&dave_directory_mutex);
    return -DAVE_HOST_EBADF;
  }
  DIR* directory = dave_directories[slot];
  dave_directories[slot] = NULL;
  pthread_mutex_unlock(&dave_directory_mutex);
  return closedir(directory) == 0 ? 0 : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemCreateDirectory(const char* path, uint32_t mode) {
  if (path == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  return mkdir(path, (mode_t)mode) == 0 ? 0 : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemRemove(const char* path) {
  if (path == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  struct stat status;
  if (lstat(path, &status) != 0) {
    return DaveHostFilesystemError();
  }
  int result = S_ISDIR(status.st_mode) ? rmdir(path) : unlink(path);
  return result == 0 ? 0 : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemRename(const char* old_path, const char* new_path) {
  if (old_path == NULL || new_path == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  return rename(old_path, new_path) == 0 ? 0 : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemCurrentPath(char* buffer, size_t capacity) {
  if (buffer == NULL || capacity == 0) {
    return -DAVE_HOST_EINVAL;
  }
  return getcwd(buffer, capacity) != NULL ? 0 : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemSetCurrentPath(const char* path) {
  if (path == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  return chdir(path) == 0 ? 0 : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemReadSymlink(const char* path, char* buffer,
                                      size_t capacity) {
  if (path == NULL || buffer == NULL || capacity == 0) {
    return -DAVE_HOST_EINVAL;
  }
  ssize_t length = readlink(path, buffer, capacity - 1);
  if (length < 0) {
    return DaveHostFilesystemError();
  }
  if ((size_t)length >= capacity - 1) {
    return -DAVE_HOST_ENAMETOOLONG;
  }
  buffer[length] = '\0';
  return (int64_t)length;
}

int64_t DaveHostFilesystemCreateSymlink(const char* target, const char* link) {
  if (target == NULL || link == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  return symlink(target, link) == 0 ? 0 : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemCreateHardLink(const char* target, const char* link) {
  if (target == NULL || link == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  return linkat(AT_FDCWD, target, AT_FDCWD, link, 0) == 0
             ? 0
             : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemSetPermissions(const char* path, uint32_t mode,
                                         int follow) {
  if (path == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  int flags = follow ? 0 : AT_SYMLINK_NOFOLLOW;
  return fchmodat(AT_FDCWD, path, (mode_t)mode, flags) == 0
             ? 0
             : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemResize(const char* path, uint64_t size) {
  if (path == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  return truncate(path, (off_t)size) == 0 ? 0 : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemSetModificationTime(const char* path,
                                              int64_t nanoseconds) {
  if (path == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  struct timespec times[2];
  times[0].tv_sec = 0;
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = (time_t)(nanoseconds / 1000000000LL);
  times[1].tv_nsec = (long)(nanoseconds % 1000000000LL);
  if (times[1].tv_nsec < 0) {
    times[1].tv_nsec += 1000000000L;
    --times[1].tv_sec;
  }
  return utimensat(AT_FDCWD, path, times, 0) == 0
             ? 0
             : DaveHostFilesystemError();
}

int64_t DaveHostFilesystemQuerySpace(const char* path,
                                     DaveHostFilesystemSpace* result) {
  if (path == NULL || result == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  struct statvfs value;
  if (statvfs(path, &value) != 0) {
    return DaveHostFilesystemError();
  }
  uint64_t block_size =
      value.f_frsize != 0 ? (uint64_t)value.f_frsize : (uint64_t)value.f_bsize;
  result->capacity = block_size * (uint64_t)value.f_blocks;
  result->free = block_size * (uint64_t)value.f_bfree;
  result->available = block_size * (uint64_t)value.f_bavail;
  return 0;
}

int64_t DaveHostFilesystemCopyFile(const char* source, const char* destination,
                                   int mode) {
  if (source == NULL || destination == NULL) {
    return -DAVE_HOST_EINVAL;
  }
  struct stat source_status;
  if (stat(source, &source_status) != 0) {
    return DaveHostFilesystemError();
  }
  struct stat destination_status;
  int destination_exists = stat(destination, &destination_status) == 0;
  if (destination_exists) {
    if ((mode & 1) != 0) {
      return 1;
    }
#if defined(__APPLE__)
    int64_t source_time =
        DaveHostFilesystemTimespecNanoseconds(source_status.st_mtimespec);
    int64_t destination_time =
        DaveHostFilesystemTimespecNanoseconds(destination_status.st_mtimespec);
#else
    int64_t source_time =
        DaveHostFilesystemTimespecNanoseconds(source_status.st_mtim);
    int64_t destination_time =
        DaveHostFilesystemTimespecNanoseconds(destination_status.st_mtim);
#endif
    if ((mode & 4) != 0 && source_time <= destination_time) {
      return 1;
    }
    if ((mode & (2 | 4)) == 0) {
      return -DAVE_HOST_EEXIST;
    }
  } else if (errno != ENOENT) {
    return DaveHostFilesystemError();
  }

  int input = open(source, O_RDONLY);
  if (input < 0) {
    return DaveHostFilesystemError();
  }
  int flags = O_WRONLY | O_CREAT;
  flags |= destination_exists ? O_TRUNC : O_EXCL;
  int output = open(destination, flags, source_status.st_mode & 07777);
  if (output < 0) {
    int64_t error = DaveHostFilesystemError();
    close(input);
    return error;
  }

  int64_t result = 0;
  char buffer[16384];
  for (;;) {
    ssize_t count;
    do {
      count = read(input, buffer, sizeof(buffer));
    } while (count < 0 && errno == EINTR);
    if (count == 0) {
      break;
    }
    if (count < 0) {
      result = DaveHostFilesystemError();
      break;
    }
    ssize_t offset = 0;
    while (offset < count) {
      ssize_t written;
      do {
        written = write(output, buffer + offset, (size_t)(count - offset));
      } while (written < 0 && errno == EINTR);
      if (written <= 0) {
        result = DaveHostFilesystemError();
        break;
      }
      offset += written;
    }
    if (result != 0) {
      break;
    }
  }
  if (close(input) != 0 && result == 0) {
    result = DaveHostFilesystemError();
  }
  if (close(output) != 0 && result == 0) {
    result = DaveHostFilesystemError();
  }
  if (result != 0 && !destination_exists) {
    unlink(destination);
  }
  return result;
}

int64_t DaveHostFilesystemCanonical(const char* path, char* buffer,
                                    size_t capacity) {
  if (path == NULL || buffer == NULL || capacity == 0) {
    return -DAVE_HOST_EINVAL;
  }
  char* resolved = realpath(path, NULL);
  if (resolved == NULL) {
    return DaveHostFilesystemError();
  }
  size_t length = strlen(resolved);
  if (length >= capacity) {
    free(resolved);
    return -DAVE_HOST_ERANGE;
  }
  memcpy(buffer, resolved, length + 1);
  free(resolved);
  return 0;
}

int64_t DaveHostEnvironmentValue(const char* name, char* buffer,
                                 size_t capacity) {
  if (name == NULL || buffer == NULL || capacity == 0) {
    return -DAVE_HOST_EINVAL;
  }
  const char* value = getenv(name);
  if (value == NULL) return 0;
  size_t length = strlen(value);
  if (length >= capacity) return -DAVE_HOST_ERANGE;
  memcpy(buffer, value, length + 1);
  return (int64_t)length + 1;
}
