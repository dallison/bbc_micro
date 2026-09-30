//
//  bbc_platform.h
//  Sockets, standard input, and the home directory differ between POSIX
//  and Windows. The emulator, cumana, and cassette use these names instead.
//
//  A socket is held in an int on both. A Winsock SOCKET is a kernel handle
//  that fits, and INVALID_SOCKET narrows to -1.
//

#ifndef bbc_platform_h
#define bbc_platform_h

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

// Winsock needs WSAStartup before the first socket call. It counts calls,
// so every entry point may make one.
static inline bool SocketStartup(void) {
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static inline int SocketClose(int fd) {
  return closesocket((SOCKET)fd);
}

static inline ssize_t SocketRead(int fd, void* buf, size_t n) {
  return recv((SOCKET)fd, (char*)buf, n > 0x7fffffff ? 0x7fffffff : (int)n, 0);
}

static inline ssize_t SocketWrite(int fd, const void* buf, size_t n) {
  return send((SOCKET)fd, (const char*)buf, n > 0x7fffffff ? 0x7fffffff : (int)n, 0);
}

static inline int SocketPoll(struct pollfd* fds, unsigned long n, int timeout_ms) {
  return WSAPoll(fds, n, timeout_ms);
}

// The call was interrupted and should be repeated. Winsock does not
// deliver signals, so this never happens there.
static inline bool SocketInterrupted(void) {
  return false;
}

static inline bool SocketWouldBlock(void) {
  int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINTR;
}

static inline int SocketSetNonBlocking(int fd) {
  u_long on = 1;
  return ioctlsocket((SOCKET)fd, FIONBIO, &on) == 0 ? 0 : -1;
}

// Standard input has a byte ready. A console, a pipe, and a redirected
// file each need a different test.
static inline bool HostStdinReady(void) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD avail = 0;
  if (in == NULL || in == INVALID_HANDLE_VALUE) {
    return false;
  }
  switch (GetFileType(in)) {
    case FILE_TYPE_CHAR:
      return _kbhit() != 0;
    case FILE_TYPE_PIPE:
      return PeekNamedPipe(in, NULL, 0, NULL, &avail, NULL) && avail != 0;
    case FILE_TYPE_DISK:
      return true;
    default:
      return false;
  }
}

// One byte from standard input when it has one ready. A console key is
// taken as it is pressed, without waiting for Return.
static inline bool HostStdinByte(uint8_t* out) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD got = 0;
  if (!HostStdinReady()) {
    return false;
  }
  if (GetFileType(in) == FILE_TYPE_CHAR) {
    *out = (uint8_t)_getch();
    return true;
  }
  return ReadFile(in, out, 1, &got, NULL) && got == 1;
}

static inline const char* HostHomeDirectory(void) {
  const char* home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    home = getenv("USERPROFILE");
  }
  return home;
}

static inline uint32_t HostProcessId(void) {
  return (uint32_t)GetCurrentProcessId();
}

#else

static inline bool SocketStartup(void) {
  return true;
}

static inline int SocketClose(int fd) {
  return close(fd);
}

static inline ssize_t SocketRead(int fd, void* buf, size_t n) {
  return read(fd, buf, n);
}

static inline ssize_t SocketWrite(int fd, const void* buf, size_t n) {
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  return send(fd, buf, n, flags);
}

static inline int SocketPoll(struct pollfd* fds, unsigned long n, int timeout_ms) {
  return poll(fds, (nfds_t)n, timeout_ms);
}

static inline bool SocketInterrupted(void) {
  return errno == EINTR;
}

static inline bool SocketWouldBlock(void) {
  return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

static inline int SocketSetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline bool HostStdinReady(void) {
  struct pollfd fd;
  fd.fd = 0;
  fd.events = POLLIN;
  fd.revents = 0;
  return poll(&fd, 1, 0) == 1;
}

static inline bool HostStdinByte(uint8_t* out) {
  return HostStdinReady() && read(0, out, 1) == 1;
}

static inline const char* HostHomeDirectory(void) {
  return getenv("HOME");
}

static inline uint32_t HostProcessId(void) {
  return (uint32_t)getpid();
}

#endif

#ifdef __cplusplus
}
#endif

#endif
