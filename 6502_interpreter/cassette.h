//
//  cassette.h
//  A cassette tape inserted into a running BBC Micro over a TCP socket.
//
//  The emulator listens on 127.0.0.1:CASSETTE_PORT. cassette connects, sends
//  one UEF (or a raw byte stream), and keeps the socket open. Closing it
//  ejects the tape. A guest recording is written back as a UEF when the
//  cassette motor stops. The client sends a single 'Z' to rewind.
//

#ifndef cassette_h
#define cassette_h

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CASSETTE_PORT 8178
#define CASSETTE_MAGIC "CASSET01"
#define CASSETTE_MAGIC_LEN 8
#define CASSETTE_MAX_NAME 255
#define CASSETTE_MAX_IMAGE (8u * 1024u * 1024u)
#define CASSETTE_FLAG_PROTECT 0x01
#define CASSETTE_OK 0
#define CASSETTE_ERR 1
#define CASSETTE_REWIND 'Z'

typedef struct CassetteInsert {
  int protect;
  char* name;
  uint8_t* data;
  size_t length;
} CassetteInsert;

typedef int (*CassetteReadFn)(void* ctx, void* buf, size_t n);

static inline int CassetteSendAll(int fd, const void* data, size_t length) {
  const uint8_t* bytes = (const uint8_t*)data;
  size_t off = 0;
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  while (off < length) {
    ssize_t n = send(fd, bytes + off, length - off, flags);
    if (n < 0) {
      if (errno == EINTR) {
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

static inline int CassetteReadAll(int fd, void* data, size_t length) {
  uint8_t* bytes = (uint8_t*)data;
  size_t off = 0;
  while (off < length) {
    ssize_t n = read(fd, bytes + off, length - off);
    if (n < 0) {
      if (errno == EINTR) {
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

static inline void CassetteInsertFree(CassetteInsert* insert) {
  if (insert == NULL) {
    return;
  }
  free(insert->name);
  free(insert->data);
  insert->name = NULL;
  insert->data = NULL;
  insert->length = 0;
}

static inline int CassettePull(CassetteReadFn fn, void* ctx, void* buf, size_t n) {
  if (n == 0) {
    return 0;
  }
  if (fn == NULL) {
    return -1;
  }
  return fn(ctx, buf, n);
}

static inline int CassetteReadInsert(CassetteReadFn fn, void* ctx, CassetteInsert* out) {
  uint8_t magic[CASSETTE_MAGIC_LEN];
  uint8_t fields[3];
  uint8_t lenb[4];
  uint16_t name_len;
  uint32_t length;
  char* name;
  uint8_t* data;
  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  if (CassettePull(fn, ctx, magic, sizeof(magic)) != 0 ||
      memcmp(magic, CASSETTE_MAGIC, CASSETTE_MAGIC_LEN) != 0) {
    return -1;
  }
  if (CassettePull(fn, ctx, fields, sizeof(fields)) != 0) {
    return -1;
  }
  name_len = (uint16_t)(((uint16_t)fields[1] << 8) | fields[2]);
  if (name_len > CASSETTE_MAX_NAME) {
    return -1;
  }
  name = (char*)malloc((size_t)name_len + 1);
  if (name == NULL) {
    return -1;
  }
  if (name_len > 0 && CassettePull(fn, ctx, name, name_len) != 0) {
    free(name);
    return -1;
  }
  name[name_len] = '\0';
  if (CassettePull(fn, ctx, lenb, sizeof(lenb)) != 0) {
    free(name);
    return -1;
  }
  length = ((uint32_t)lenb[0] << 24) | ((uint32_t)lenb[1] << 16) |
           ((uint32_t)lenb[2] << 8) | (uint32_t)lenb[3];
  if (length > CASSETTE_MAX_IMAGE) {
    free(name);
    return -1;
  }
  data = NULL;
  if (length > 0) {
    data = (uint8_t*)malloc(length);
    if (data == NULL || CassettePull(fn, ctx, data, length) != 0) {
      free(name);
      free(data);
      return -1;
    }
  }
  out->protect = (fields[0] & CASSETTE_FLAG_PROTECT) != 0;
  out->name = name;
  out->data = data;
  out->length = length;
  return 0;
}

static inline int CassetteSendInsert(int fd, int protect, const char* name, const uint8_t* data,
                                     size_t length) {
  uint8_t header[CASSETTE_MAGIC_LEN + 3];
  uint8_t lenb[4];
  size_t name_len;
  if (name == NULL || length > CASSETTE_MAX_IMAGE || (data == NULL && length != 0)) {
    return -1;
  }
  name_len = strlen(name);
  if (name_len > CASSETTE_MAX_NAME) {
    name_len = CASSETTE_MAX_NAME;
  }
  memcpy(header, CASSETTE_MAGIC, CASSETTE_MAGIC_LEN);
  header[CASSETTE_MAGIC_LEN] = protect ? (uint8_t)CASSETTE_FLAG_PROTECT : 0;
  header[CASSETTE_MAGIC_LEN + 1] = (uint8_t)(name_len >> 8);
  header[CASSETTE_MAGIC_LEN + 2] = (uint8_t)name_len;
  lenb[0] = (uint8_t)(length >> 24);
  lenb[1] = (uint8_t)(length >> 16);
  lenb[2] = (uint8_t)(length >> 8);
  lenb[3] = (uint8_t)length;
  if (CassetteSendAll(fd, header, sizeof(header)) != 0) {
    return -1;
  }
  if (name_len > 0 && CassetteSendAll(fd, name, name_len) != 0) {
    return -1;
  }
  if (CassetteSendAll(fd, lenb, sizeof(lenb)) != 0) {
    return -1;
  }
  if (length > 0 && CassetteSendAll(fd, data, length) != 0) {
    return -1;
  }
  return 0;
}

static inline int CassetteSendReply(int fd, int code, const char* msg) {
  uint8_t header[4];
  size_t n = msg == NULL ? 0 : strlen(msg);
  if (n > 512) {
    n = 512;
  }
  header[0] = (uint8_t)code;
  header[1] = 0;
  header[2] = (uint8_t)(n >> 8);
  header[3] = (uint8_t)n;
  if (CassetteSendAll(fd, header, sizeof(header)) != 0) {
    return -1;
  }
  if (n > 0 && CassetteSendAll(fd, msg, n) != 0) {
    return -1;
  }
  return 0;
}

static inline int CassetteReadReply(int fd, int* code, char* msg, size_t msg_cap) {
  uint8_t header[4];
  uint16_t n;
  if (CassetteReadAll(fd, header, sizeof(header)) != 0) {
    return -1;
  }
  n = (uint16_t)(((uint16_t)header[2] << 8) | header[3]);
  if (n > 512) {
    return -1;
  }
  if (n > 0) {
    char tmp[512];
    if (CassetteReadAll(fd, tmp, n) != 0) {
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
  return 0;
}

static inline int CassetteSendImage(int fd, const uint8_t* data, size_t length) {
  uint8_t header[5];
  if (data == NULL && length != 0) {
    return -1;
  }
  header[0] = 'W';
  header[1] = (uint8_t)(length >> 24);
  header[2] = (uint8_t)(length >> 16);
  header[3] = (uint8_t)(length >> 8);
  header[4] = (uint8_t)length;
  if (CassetteSendAll(fd, header, sizeof(header)) != 0) {
    return -1;
  }
  if (length > 0 && CassetteSendAll(fd, data, length) != 0) {
    return -1;
  }
  return 0;
}

static inline int CassetteReadImage(int fd, uint8_t** data, size_t* length) {
  uint8_t header[5];
  uint32_t n;
  uint8_t* buf;
  if (data == NULL || length == NULL) {
    return -1;
  }
  *data = NULL;
  *length = 0;
  if (CassetteReadAll(fd, header, sizeof(header)) != 0 || header[0] != 'W') {
    return -1;
  }
  n = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) |
      ((uint32_t)header[3] << 8) | (uint32_t)header[4];
  if (n > CASSETTE_MAX_IMAGE) {
    return -1;
  }
  buf = n == 0 ? NULL : (uint8_t*)malloc(n);
  if (n > 0 && (buf == NULL || CassetteReadAll(fd, buf, n) != 0)) {
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
