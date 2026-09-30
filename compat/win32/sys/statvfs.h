//
//  sys/statvfs.h for MinGW. Only the Windows build puts compat/win32 on the
//  include path. The fields are those the davecc filesystem host reads.
//

#ifndef BBC_COMPAT_SYS_STATVFS_H
#define BBC_COMPAT_SYS_STATVFS_H

#include <errno.h>
#include <windows.h>

struct statvfs {
  unsigned long f_bsize;
  unsigned long f_frsize;
  unsigned long long f_blocks;
  unsigned long long f_bfree;
  unsigned long long f_bavail;
};

static inline int statvfs(const char* path, struct statvfs* out) {
  ULARGE_INTEGER available;
  ULARGE_INTEGER total;
  ULARGE_INTEGER free_bytes;
  char full[MAX_PATH];
  char* file_part = NULL;
  DWORD n;
  if (path == NULL || out == NULL) {
    errno = EINVAL;
    return -1;
  }
  n = GetFullPathNameA(path, MAX_PATH, full, &file_part);
  if (n == 0 || n >= MAX_PATH ||
      !GetDiskFreeSpaceExA(full, &available, &total, &free_bytes)) {
    errno = ENOENT;
    return -1;
  }
  out->f_bsize = 1;
  out->f_frsize = 1;
  out->f_blocks = total.QuadPart;
  out->f_bfree = free_bytes.QuadPart;
  out->f_bavail = available.QuadPart;
  return 0;
}

#endif
