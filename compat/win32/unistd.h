//
//  unistd.h for MinGW. Only the Windows build puts compat/win32 on the
//  include path. MinGW's own unistd.h comes first; this adds the POSIX
//  calls the davecc loader and host services expect from unistd.h and
//  stdlib.h that MinGW does not provide.
//

#ifndef BBC_COMPAT_UNISTD_H
#define BBC_COMPAT_UNISTD_H

#include_next <unistd.h>

#include <errno.h>
#include <io.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

// The CRT has no positional read. The file offset is restored afterwards,
// but another thread using the same descriptor meanwhile would see it move.
static inline ssize_t pread(int fd, void* buf, size_t n, off_t offset) {
  long long saved = _lseeki64(fd, 0, SEEK_CUR);
  int got;
  if (saved < 0 || _lseeki64(fd, (long long)offset, SEEK_SET) < 0) {
    return -1;
  }
  got = _read(fd, buf, n > (size_t)INT_MAX ? (unsigned)INT_MAX : (unsigned)n);
  _lseeki64(fd, saved, SEEK_SET);
  return got;
}

// Windows has symbolic links, but creating one needs a privilege most
// accounts lack. The guest sees ENOSYS, as it would on a file system
// without links.
static inline ssize_t readlink(const char* path, char* buf, size_t n) {
  (void)path;
  (void)buf;
  (void)n;
  errno = EINVAL;
  return -1;
}

static inline int symlink(const char* target, const char* link) {
  (void)target;
  (void)link;
  errno = ENOSYS;
  return -1;
}

static inline char* realpath(const char* path, char* resolved) {
  return _fullpath(resolved, path, resolved != NULL ? PATH_MAX : 0);
}

// The block is released with free(), so only what malloc already
// guarantees can be offered.
static inline int posix_memalign(void** out, size_t align, size_t size) {
  void* block;
  if (align > 16 || (align & (align - 1)) != 0) {
    return EINVAL;
  }
  block = malloc(size);
  if (block == NULL) {
    return ENOMEM;
  }
  *out = block;
  return 0;
}

#endif
