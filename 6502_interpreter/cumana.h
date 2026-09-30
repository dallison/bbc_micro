//
//  cumana.h
//  Disc images inserted into a running BBC Micro over a TCP socket.
//
//  The emulator listens on 127.0.0.1:CUMANA_PORT. cumana connects, sends one
//  image, and keeps the socket open. Closing it ejects the disc. Guest
//  writes come back as W packets and replace the file.
//
//  DFS uses drives 0-3 (2 and 3 are the second side of drives 0 and 1).
//  ADFS uses drives 0 and 1. A double-sided image is one of those drives.
//

#ifndef cumana_h
#define cumana_h

#include "bbc_platform.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUMANA_PORT 8177
#define CUMANA_MAGIC "CUMANA01"
#define CUMANA_MAGIC_LEN 8
#define CUMANA_DRIVE_AUTO 0xff
#define CUMANA_MAX_NAME 255
#define CUMANA_MAX_IMAGE (32u * 1024u * 1024u)
#define CUMANA_FLAG_PROTECT 0x01

// 0 accepts the image. Anything else is a refusal; the socket then closes.
#define CUMANA_OK 0
#define CUMANA_ERR 1

typedef struct CumanaInsert {
  int drive;  // 0-3, or -1 when the emulator should pick a free drive.
  int protect;
  char* name;
  uint8_t* data;
  size_t length;
} CumanaInsert;

// Reads exactly n bytes. Returns 0, or -1 on EOF or error.
typedef int (*CumanaReadFn)(void* ctx, void* buf, size_t n);

static inline int CumanaSendAll(int fd, const void* data, size_t length) {
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

static inline int CumanaReadAll(int fd, void* data, size_t length) {
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

static inline int CumanaReadFromFd(void* ctx, void* buf, size_t n) {
  int fd = *(int*)ctx;
  return CumanaReadAll(fd, buf, n);
}

static inline void CumanaInsertFree(CumanaInsert* insert) {
  if (insert == NULL) {
    return;
  }
  free(insert->name);
  free(insert->data);
  insert->name = NULL;
  insert->data = NULL;
  insert->length = 0;
}

static inline int CumanaPull(CumanaReadFn fn, void* ctx, void* buf, size_t n) {
  if (fn == NULL || n == 0) {
    return n == 0 ? 0 : -1;
  }
  return fn(ctx, buf, n);
}

static inline int CumanaReadInsert(CumanaReadFn fn, void* ctx, CumanaInsert* out) {
  uint8_t magic[CUMANA_MAGIC_LEN];
  uint8_t fields[4];
  uint8_t lenb[4];
  uint16_t name_len;
  uint32_t length;
  char* name;
  uint8_t* data;
  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->drive = -1;
  if (CumanaPull(fn, ctx, magic, sizeof(magic)) != 0 ||
      memcmp(magic, CUMANA_MAGIC, CUMANA_MAGIC_LEN) != 0) {
    return -1;
  }
  if (CumanaPull(fn, ctx, fields, sizeof(fields)) != 0) {
    return -1;
  }
  name_len = (uint16_t)(((uint16_t)fields[2] << 8) | fields[3]);
  if (name_len > CUMANA_MAX_NAME) {
    return -1;
  }
  name = (char*)malloc((size_t)name_len + 1);
  if (name == NULL) {
    return -1;
  }
  if (name_len > 0 && CumanaPull(fn, ctx, name, name_len) != 0) {
    free(name);
    return -1;
  }
  name[name_len] = '\0';
  if (CumanaPull(fn, ctx, lenb, sizeof(lenb)) != 0) {
    free(name);
    return -1;
  }
  length = ((uint32_t)lenb[0] << 24) | ((uint32_t)lenb[1] << 16) |
           ((uint32_t)lenb[2] << 8) | (uint32_t)lenb[3];
  if (length == 0 || length > CUMANA_MAX_IMAGE) {
    free(name);
    return -1;
  }
  data = (uint8_t*)malloc(length);
  if (data == NULL || CumanaPull(fn, ctx, data, length) != 0) {
    free(name);
    free(data);
    return -1;
  }
  out->drive = fields[0] == CUMANA_DRIVE_AUTO ? -1 : (int)fields[0];
  out->protect = (fields[1] & CUMANA_FLAG_PROTECT) != 0;
  out->name = name;
  out->data = data;
  out->length = length;
  return 0;
}

static inline int CumanaSendInsert(int fd, int drive, int protect, const char* name,
                                   const uint8_t* data, size_t length) {
  uint8_t header[CUMANA_MAGIC_LEN + 4];
  uint8_t lenb[4];
  size_t name_len;
  if (name == NULL || data == NULL || length == 0 || length > CUMANA_MAX_IMAGE) {
    return -1;
  }
  name_len = strlen(name);
  if (name_len > CUMANA_MAX_NAME) {
    name_len = CUMANA_MAX_NAME;
  }
  memcpy(header, CUMANA_MAGIC, CUMANA_MAGIC_LEN);
  header[CUMANA_MAGIC_LEN] = drive < 0 ? (uint8_t)CUMANA_DRIVE_AUTO : (uint8_t)drive;
  header[CUMANA_MAGIC_LEN + 1] = protect ? (uint8_t)CUMANA_FLAG_PROTECT : 0;
  header[CUMANA_MAGIC_LEN + 2] = (uint8_t)(name_len >> 8);
  header[CUMANA_MAGIC_LEN + 3] = (uint8_t)name_len;
  lenb[0] = (uint8_t)(length >> 24);
  lenb[1] = (uint8_t)(length >> 16);
  lenb[2] = (uint8_t)(length >> 8);
  lenb[3] = (uint8_t)length;
  if (CumanaSendAll(fd, header, sizeof(header)) != 0) {
    return -1;
  }
  if (name_len > 0 && CumanaSendAll(fd, name, name_len) != 0) {
    return -1;
  }
  if (CumanaSendAll(fd, lenb, sizeof(lenb)) != 0 || CumanaSendAll(fd, data, length) != 0) {
    return -1;
  }
  return 0;
}

static inline int CumanaSendReply(int fd, int code, int drive, const char* msg) {
  uint8_t header[4];
  size_t n = msg == NULL ? 0 : strlen(msg);
  if (n > 512) {
    n = 512;
  }
  header[0] = (uint8_t)code;
  header[1] = drive < 0 ? (uint8_t)CUMANA_DRIVE_AUTO : (uint8_t)drive;
  header[2] = (uint8_t)(n >> 8);
  header[3] = (uint8_t)n;
  if (CumanaSendAll(fd, header, sizeof(header)) != 0) {
    return -1;
  }
  if (n > 0 && CumanaSendAll(fd, msg, n) != 0) {
    return -1;
  }
  return 0;
}

static inline int CumanaReadReply(int fd, int* code, int* drive, char* msg, size_t msg_cap) {
  uint8_t header[4];
  uint16_t n;
  if (CumanaReadAll(fd, header, sizeof(header)) != 0) {
    return -1;
  }
  n = (uint16_t)(((uint16_t)header[2] << 8) | header[3]);
  if (n > 512) {
    return -1;
  }
  if (n > 0) {
    char tmp[512];
    if (CumanaReadAll(fd, tmp, n) != 0) {
      return -1;
    }
    if (msg != NULL && msg_cap > 0) {
      size_t copy = n;
      if (copy >= msg_cap) {
        copy = msg_cap - 1;
      }
      memcpy(msg, tmp, copy);
      msg[copy] = '\0';
    }
  } else if (msg != NULL && msg_cap > 0) {
    msg[0] = '\0';
  }
  if (code != NULL) {
    *code = header[0];
  }
  if (drive != NULL) {
    *drive = header[1] == CUMANA_DRIVE_AUTO ? -1 : (int)header[1];
  }
  return 0;
}

// One guest write-back. The tag byte is 'W'.
static inline int CumanaSendImage(int fd, const uint8_t* data, size_t length) {
  uint8_t header[5];
  if (data == NULL && length != 0) {
    return -1;
  }
  header[0] = 'W';
  header[1] = (uint8_t)(length >> 24);
  header[2] = (uint8_t)(length >> 16);
  header[3] = (uint8_t)(length >> 8);
  header[4] = (uint8_t)length;
  if (CumanaSendAll(fd, header, sizeof(header)) != 0) {
    return -1;
  }
  if (length > 0 && CumanaSendAll(fd, data, length) != 0) {
    return -1;
  }
  return 0;
}

static inline int CumanaReadImage(int fd, uint8_t** data, size_t* length) {
  uint8_t header[5];
  uint32_t n;
  uint8_t* buf;
  if (data == NULL || length == NULL) {
    return -1;
  }
  *data = NULL;
  *length = 0;
  if (CumanaReadAll(fd, header, sizeof(header)) != 0 || header[0] != 'W') {
    return -1;
  }
  n = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) |
      ((uint32_t)header[3] << 8) | (uint32_t)header[4];
  if (n > CUMANA_MAX_IMAGE) {
    return -1;
  }
  buf = n == 0 ? NULL : (uint8_t*)malloc(n);
  if (n > 0 && (buf == NULL || CumanaReadAll(fd, buf, n) != 0)) {
    free(buf);
    return -1;
  }
  *data = buf;
  *length = n;
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif
