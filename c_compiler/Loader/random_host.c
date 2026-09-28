#include "random_host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

int DaveHostRandomBytes(void* buffer, size_t size) {
  if (size != 0 && buffer == NULL) {
    return -1;
  }
  if (size == 0) {
    return 0;
  }

  int fd;
  do {
    fd = open("/dev/urandom", O_RDONLY);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return -1;
  }

  uint8_t* output = buffer;
  size_t remaining = size;
  while (remaining != 0) {
    ssize_t count = read(fd, output, remaining);
    if (count > 0) {
      output += count;
      remaining -= (size_t)count;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    close(fd);
    return -1;
  }

  if (close(fd) < 0 && errno != EINTR) {
    return -1;
  }
  return 0;
}
