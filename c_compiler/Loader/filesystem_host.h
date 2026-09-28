#ifndef filesystem_host_h
#define filesystem_host_h

#include <stddef.h>
#include <stdint.h>

// Stable wire structures shared by the host interpreters.  The guest runtime
// has matching private definitions; no host struct stat/dirent layout crosses
// the interpreter boundary.
typedef struct {
  uint64_t device;
  uint64_t inode;
  uint64_t size;
  uint64_t hard_link_count;
  int64_t access_time_ns;
  int64_t modification_time_ns;
  int64_t status_change_time_ns;
  uint32_t mode;
  uint32_t reserved;
} DaveHostFilesystemStat;

enum { DAVE_HOST_FILESYSTEM_NAME_MAX = 1024 };

typedef struct {
  uint32_t type;
  uint32_t reserved;
  char name[DAVE_HOST_FILESYSTEM_NAME_MAX];
} DaveHostFilesystemDirectoryEntry;

typedef struct {
  uint64_t capacity;
  uint64_t free;
  uint64_t available;
} DaveHostFilesystemSpace;

// DaveCC's guest errno values are deliberately platform-neutral and do not
// match either Darwin or Linux.  Host filesystem calls translate into this
// stable subset before returning through the guest syscall ABI.
enum {
  DAVE_HOST_ENOENT = 1,
  DAVE_HOST_ENOMEM = 2,
  DAVE_HOST_EACCES = 3,
  DAVE_HOST_ENODEV = 4,
  DAVE_HOST_EMFILE = 5,
  DAVE_HOST_EBUSY = 6,
  DAVE_HOST_EINVAL = 7,
  DAVE_HOST_ENOSPC = 8,
  DAVE_HOST_EEXIST = 9,
  DAVE_HOST_EAGAIN = 10,
  DAVE_HOST_EIO = 11,
  DAVE_HOST_EINTR = 12,
  DAVE_HOST_ENOSYS = 13,
  DAVE_HOST_ESPIPE = 14,
  DAVE_HOST_ERANGE = 15,
  DAVE_HOST_EBADF = 16,
  DAVE_HOST_ENOEXEC = 17,
  DAVE_HOST_EUNKNOWN = 18,
  DAVE_HOST_EPERM = 19,
  DAVE_HOST_ESRCH = 20,
  DAVE_HOST_EDEADLK = 21,
  DAVE_HOST_ENOTDIR = 22,
  DAVE_HOST_EISDIR = 23,
  DAVE_HOST_ENAMETOOLONG = 24,
  DAVE_HOST_ENOTEMPTY = 25,
  DAVE_HOST_ELOOP = 26,
  DAVE_HOST_EROFS = 27,
  DAVE_HOST_EXDEV = 28,
  DAVE_HOST_ENOTSUP = 29,
  DAVE_HOST_EFBIG = 30,
  DAVE_HOST_ENFILE = 31,
};

int64_t DaveHostFilesystemGetStatus(const char* path, int follow,
                                    DaveHostFilesystemStat* result);
int64_t DaveHostFilesystemGetDescriptorStatus(
    int fd, DaveHostFilesystemStat* result);
int64_t DaveHostFilesystemOpenDirectory(const char* path);
int64_t DaveHostFilesystemReadDirectory(
    int handle, DaveHostFilesystemDirectoryEntry* result);
int64_t DaveHostFilesystemCloseDirectory(int handle);
int64_t DaveHostFilesystemCreateDirectory(const char* path, uint32_t mode);
int64_t DaveHostFilesystemRemove(const char* path);
int64_t DaveHostFilesystemRename(const char* old_path, const char* new_path);
int64_t DaveHostFilesystemCurrentPath(char* buffer, size_t capacity);
int64_t DaveHostFilesystemSetCurrentPath(const char* path);
int64_t DaveHostFilesystemReadSymlink(const char* path, char* buffer,
                                      size_t capacity);
int64_t DaveHostFilesystemCreateSymlink(const char* target, const char* link);
int64_t DaveHostFilesystemCreateHardLink(const char* target, const char* link);
int64_t DaveHostFilesystemSetPermissions(const char* path, uint32_t mode,
                                         int follow);
int64_t DaveHostFilesystemResize(const char* path, uint64_t size);
int64_t DaveHostFilesystemSetModificationTime(const char* path,
                                              int64_t nanoseconds);
int64_t DaveHostFilesystemQuerySpace(const char* path,
                                     DaveHostFilesystemSpace* result);
int64_t DaveHostFilesystemCopyFile(const char* source, const char* destination,
                                   int mode);
int64_t DaveHostFilesystemCanonical(const char* path, char* buffer,
                                    size_t capacity);
int64_t DaveHostEnvironmentValue(const char* name, char* buffer,
                                 size_t capacity);

#endif
