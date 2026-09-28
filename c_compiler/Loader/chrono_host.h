#ifndef chrono_host_h
#define chrono_host_h

#include <stddef.h>
#include <stdint.h>

#include "filesystem_host.h"

// Stable wire structures shared by the host interpreters.  Guest tests and the
// future libc chrono runtime have matching private definitions; no host TZif
// layout crosses the interpreter boundary.
enum { DAVE_HOST_CHRONO_NAME_MAX = 256 };
enum { DAVE_HOST_CHRONO_ABBREV_MAX = 16 };

typedef struct {
  int64_t begin_seconds;
  int64_t end_seconds;
  int32_t offset_seconds;
  uint32_t flags;
  uint32_t reserved;
} DaveHostChronoSysInfoWire;

enum {
  DAVE_HOST_CHRONO_LOCAL_UNIQUE = 0,
  DAVE_HOST_CHRONO_LOCAL_NONEXISTENT = 1,
  DAVE_HOST_CHRONO_LOCAL_AMBIGUOUS = 2,
};

typedef struct {
  int32_t result;
  uint32_t reserved;
  DaveHostChronoSysInfoWire first;
  DaveHostChronoSysInfoWire second;
} DaveHostChronoLocalInfoWire;

typedef struct {
  int64_t date_seconds;
  int32_t correction_seconds;
  uint32_t reserved;
} DaveHostChronoLeapSecond;

typedef struct {
  uint64_t seconds_address;
  uint64_t result_address;
  uint64_t abbrev_address;
  uint32_t abbrev_capacity;
  uint32_t reserved;
} DaveHostChronoSysInfoRequestWire;

typedef struct {
  uint64_t seconds_address;
  uint64_t result_address;
  uint64_t abbrev_address;
  uint32_t abbrev_capacity;
  uint32_t reserved;
} DaveHostChronoLocalInfoRequestWire;

int64_t DaveHostChronoTzdbVersion(char* buffer, size_t capacity);
int64_t DaveHostChronoGeneration(uint64_t* generation);
int64_t DaveHostChronoReload(uint64_t* generation);
int64_t DaveHostChronoCurrentZone(char* buffer, size_t capacity);
int64_t DaveHostChronoZoneCount(uint32_t* count);
int64_t DaveHostChronoZoneName(uint32_t index, char* buffer, size_t capacity);
int64_t DaveHostChronoLocateZone(const char* name, char* buffer, size_t capacity,
                                 uint32_t* index);
int64_t DaveHostChronoSysInfo(const char* zone_name, int64_t sys_seconds,
                              DaveHostChronoSysInfoWire* result,
                              char* abbrev_buffer, size_t abbrev_capacity);
int64_t DaveHostChronoSysInfoRequest(const char* zone_name,
                                     const DaveHostChronoSysInfoRequestWire* request);
int64_t DaveHostChronoLocalInfo(const char* zone_name, int64_t local_seconds,
                                DaveHostChronoLocalInfoWire* result,
                                char* first_abbrev_buffer,
                                size_t first_abbrev_capacity,
                                char* second_abbrev_buffer,
                                size_t second_abbrev_capacity);
int64_t DaveHostChronoLocalInfoRequest(
    const char* zone_name, const DaveHostChronoLocalInfoRequestWire* request);
int64_t DaveHostChronoLeapCount(uint32_t* count);
int64_t DaveHostChronoLeapInfo(uint32_t index, DaveHostChronoLeapSecond* result);

#endif
