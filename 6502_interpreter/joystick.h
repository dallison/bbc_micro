//
//  joystick.h
//  Analogue and digital joystick input for a running BBC Micro over TCP.
//
//  The emulator listens on 127.0.0.1:JOYSTICK_PORT. The joystick program
//  connects, sends BBCJOY01, then streams JoystickPacket records at about
//  50 Hz until it disconnects. Closing the connection recentres the sticks.
//
//  Analogue channels 0 and 1 are the µPD7002 X and Y inputs (0–65535,
//  centre 0x8000). Fire buttons map to the system VIA on PB4 and PB5.
//  The digital field drives the user VIA PB0–PB3 (right, left, down, up),
//  active low, for digital sticks and games that read the user port.
//

#ifndef joystick_h
#define joystick_h

#include "bbc_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// 8179 is the default Econet UDP port; keep joystick on its own TCP port.
#define JOYSTICK_PORT 8180
#define JOYSTICK_MAGIC "BBCJOY01"
#define JOYSTICK_MAGIC_LEN 8
#define JOYSTICK_VERSION 1

#define JOYSTICK_OK 0
#define JOYSTICK_ERR 1

#define JOYSTICK_DIGITAL_RIGHT 0x01
#define JOYSTICK_DIGITAL_LEFT 0x02
#define JOYSTICK_DIGITAL_DOWN 0x04
#define JOYSTICK_DIGITAL_UP 0x08

typedef struct JoystickPacket {
  uint16_t x;
  uint16_t y;
  uint8_t fire0;
  uint8_t fire1;
  uint8_t digital;
  uint8_t pad;
} JoystickPacket;

static inline int JoystickSendAll(int fd, const void* data, size_t length) {
  const uint8_t* bytes = (const uint8_t*)data;
  size_t off = 0;
  while (off < length) {
    ssize_t n = SocketWrite(fd, bytes + off, length - off);
    if (n < 0) {
      if (SocketInterrupted()) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

static inline int JoystickReadAll(int fd, void* data, size_t length) {
  uint8_t* bytes = (uint8_t*)data;
  size_t off = 0;
  while (off < length) {
    ssize_t n = SocketRead(fd, bytes + off, length - off);
    if (n < 0) {
      if (SocketInterrupted()) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

static inline int JoystickConnect(const char* host, int port) {
  struct sockaddr_in addr;
  int fd;
  uint32_t version;
  uint32_t reply;
  if (host == NULL || port < 1 || port > 65535) {
    return -1;
  }
  if (!SocketStartup()) {
    return -1;
  }
  fd = (int)socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  {
    int one = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    SocketClose(fd);
    return -1;
  }
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    SocketClose(fd);
    return -1;
  }
  if (JoystickSendAll(fd, JOYSTICK_MAGIC, JOYSTICK_MAGIC_LEN) != 0) {
    SocketClose(fd);
    return -1;
  }
  version = JOYSTICK_VERSION;
  if (JoystickSendAll(fd, &version, sizeof(version)) != 0) {
    SocketClose(fd);
    return -1;
  }
  if (JoystickReadAll(fd, &reply, sizeof(reply)) != 0) {
    SocketClose(fd);
    return -1;
  }
  if (reply != JOYSTICK_OK) {
    SocketClose(fd);
    return -1;
  }
  return fd;
}

static inline int JoystickSendPacket(int fd, const JoystickPacket* packet) {
  if (fd < 0 || packet == NULL) {
    return -1;
  }
  return JoystickSendAll(fd, packet, sizeof(*packet));
}

#ifdef __cplusplus
}
#endif

#endif /* joystick_h */
