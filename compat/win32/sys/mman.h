//
//  sys/mman.h for MinGW. Only the Windows build puts compat/win32 on the
//  include path.
//
//  Memory comes from VirtualAlloc. A file mapping is a private copy read
//  from the file, which is all MAP_PRIVATE promises; MAP_SHARED is not
//  supported. PROT_* is accepted and every page stays read-write.
//

#ifndef BBC_COMPAT_SYS_MMAN_H
#define BBC_COMPAT_SYS_MMAN_H

#include <errno.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <windows.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON

#define MAP_FAILED ((void*)-1)

#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#endif

// Callers align MAP_FIXED addresses to this. VirtualAlloc reserves in
// allocation-granularity units, which are larger than a page.
static inline long sysconf(int name) {
  SYSTEM_INFO info;
  if (name != _SC_PAGESIZE) {
    errno = EINVAL;
    return -1;
  }
  GetSystemInfo(&info);
  return (long)info.dwAllocationGranularity;
}

static inline void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  void* base;
  (void)prot;
  if (length == 0 || (flags & MAP_SHARED) != 0) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  if ((flags & MAP_FIXED) != 0) {
    base = VirtualAlloc(addr, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base == NULL) {
      // Inside an earlier mapping the range is already reserved.
      base = VirtualAlloc(addr, length, MEM_COMMIT, PAGE_READWRITE);
    }
    if (base != NULL) {
      memset(base, 0, length);
    }
  } else {
    base = VirtualAlloc(NULL, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  }
  if (base == NULL) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  if ((flags & MAP_ANON) == 0) {
    uint8_t* bytes = (uint8_t*)base;
    size_t off = 0;
    if (_lseeki64(fd, (long long)offset, SEEK_SET) < 0) {
      VirtualFree(base, 0, MEM_RELEASE);
      errno = EBADF;
      return MAP_FAILED;
    }
    while (off < length) {
      unsigned chunk = length - off > 0x40000000u ? 0x40000000u : (unsigned)(length - off);
      int got = _read(fd, bytes + off, chunk);
      if (got < 0) {
        VirtualFree(base, 0, MEM_RELEASE);
        return MAP_FAILED;
      }
      if (got == 0) {
        break;
      }
      off += (size_t)got;
    }
  }
  return base;
}

static inline int munmap(void* addr, size_t length) {
  if (VirtualFree(addr, 0, MEM_RELEASE)) {
    return 0;
  }
  // Part of a larger mapping.
  if (VirtualFree(addr, length, MEM_DECOMMIT)) {
    return 0;
  }
  errno = EINVAL;
  return -1;
}

static inline int mprotect(void* addr, size_t length, int prot) {
  (void)addr;
  (void)length;
  (void)prot;
  return 0;
}

#endif
