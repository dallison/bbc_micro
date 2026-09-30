//
//  cumana.c
//  Insert a disc image into a running bbc emulator.
//
//    cumana blank.ssd
//    cumana -drive 2 side.ssd
//    cumana -drive 0 blank.adl
//

#include "cumana.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void Usage(void) {
  fprintf(stderr,
          "usage: cumana [-drive 0-3] [-host addr] [-port n] [-ro] image\n"
          "       Connects to a running bbc emulator and inserts the image.\n"
          "       DFS drives are 0-3. Drives 2 and 3 are the second side of\n"
          "       drives 0 and 1. ADFS uses drives 0 and 1; a double-sided\n"
          "       image (.dsd or .adl) is one of those drives.\n"
          "       With no -drive, the emulator uses the first free drive.\n"
          "       The default address is 127.0.0.1:%d. The program stays\n"
          "       connected, writes guest changes back to the file, and\n"
          "       ejects the disc when it exits.\n",
          CUMANA_PORT);
}

static int ParsePort(const char* text) {
  char* end = NULL;
  long value;
  if (text == NULL || text[0] == '\0') {
    return -1;
  }
  value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 1 || value > 65535) {
    return -1;
  }
  return (int)value;
}

static int ParseDrive(const char* text) {
  if (text == NULL || text[1] != '\0' || text[0] < '0' || text[0] > '3') {
    return -1;
  }
  return text[0] - '0';
}

static const char* BaseName(const char* path) {
  const char* slash = strrchr(path, '/');
#ifdef _WIN32
  const char* backslash = strrchr(path, '\\');
  if (backslash != NULL && (slash == NULL || backslash > slash)) {
    slash = backslash;
  }
#endif
  if (slash != NULL && slash[1] != '\0') {
    return slash + 1;
  }
  return path;
}

static int ReadWholeFile(const char* path, uint8_t** data, size_t* length) {
  FILE* fp;
  long size;
  uint8_t* buf;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  size = ftell(fp);
  if (size <= 0 || (unsigned long)size > CUMANA_MAX_IMAGE) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  buf = (uint8_t*)malloc((size_t)size);
  if (buf == NULL || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
    free(buf);
    fclose(fp);
    return -1;
  }
  fclose(fp);
  *data = buf;
  *length = (size_t)size;
  return 0;
}

static int ConnectTo(const char* host, int port) {
  int fd;
  struct sockaddr_in addr;
  int one = 1;
  if (!SocketStartup()) {
    return -1;
  }
  fd = (int)socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
      connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    SocketClose(fd);
    return -1;
  }
  return fd;
}

static int WriteBack(FILE* fp, const uint8_t* data, size_t length) {
  if (fp == NULL) {
    return 0;
  }
  if (fseek(fp, 0, SEEK_SET) != 0 || fwrite(data, 1, length, fp) != length || fflush(fp) != 0) {
    return -1;
  }
  if (ftruncate(fileno(fp), (off_t)length) != 0) {
    return -1;
  }
  return 0;
}

int main(int argc, char** argv) {
  const char* host = "127.0.0.1";
  const char* path = NULL;
  int port = CUMANA_PORT;
  int drive = -1;
  int protect = 0;
  int fd;
  int code = CUMANA_ERR;
  int assigned = -1;
  uint8_t* image = NULL;
  size_t length = 0;
  char reply[513];
  FILE* fp = NULL;
  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-drive") == 0) {
      if (i + 1 >= argc) {
        Usage();
        return 1;
      }
      drive = ParseDrive(argv[++i]);
      if (drive < 0) {
        Usage();
        return 1;
      }
    } else if (strcmp(argv[i], "-host") == 0) {
      if (i + 1 >= argc) {
        Usage();
        return 1;
      }
      host = argv[++i];
    } else if (strcmp(argv[i], "-port") == 0) {
      if (i + 1 >= argc) {
        Usage();
        return 1;
      }
      port = ParsePort(argv[++i]);
      if (port < 0) {
        Usage();
        return 1;
      }
    } else if (strcmp(argv[i], "-ro") == 0) {
      protect = 1;
    } else if (strcmp(argv[i], "-h") == 0) {
      Usage();
      return 0;
    } else if (argv[i][0] == '-') {
      Usage();
      return 1;
    } else if (path != NULL) {
      Usage();
      return 1;
    } else {
      path = argv[i];
    }
  }
  if (path == NULL) {
    Usage();
    return 1;
  }
  if (access(path, W_OK) != 0) {
    protect = 1;
  }
  if (ReadWholeFile(path, &image, &length) != 0) {
    fprintf(stderr, "Unable to read %s\n", path);
    return 1;
  }
  fd = ConnectTo(host, port);
  if (fd < 0) {
    fprintf(stderr, "Unable to connect to %s:%d\n", host, port);
    free(image);
    return 1;
  }
  if (CumanaSendInsert(fd, drive, protect, BaseName(path), image, length) != 0 ||
      CumanaReadReply(fd, &code, &assigned, reply, sizeof(reply)) != 0) {
    fprintf(stderr, "The emulator did not accept %s\n", path);
    SocketClose(fd);
    free(image);
    return 1;
  }
  if (code != CUMANA_OK) {
    fprintf(stderr, "Refused: %s\n", reply[0] != '\0' ? reply : "disc not inserted");
    SocketClose(fd);
    free(image);
    return 1;
  }
  fprintf(stderr, "Drive %d: %s\n", assigned, path);
  free(image);
  image = NULL;
  if (!protect) {
    fp = fopen(path, "r+b");
    if (fp == NULL) {
      fprintf(stderr, "Unable to write %s; the disc is mounted read-only\n", path);
    }
  }
  for (;;) {
    uint8_t* written = NULL;
    size_t written_len = 0;
    if (CumanaReadImage(fd, &written, &written_len) != 0) {
      break;
    }
    if (WriteBack(fp, written, written_len) != 0) {
      fprintf(stderr, "Unable to update %s\n", path);
      free(written);
      break;
    }
    free(written);
  }
  if (fp != NULL) {
    fclose(fp);
  }
  SocketClose(fd);
  fprintf(stderr, "Drive %d ejected\n", assigned);
  return 0;
}
