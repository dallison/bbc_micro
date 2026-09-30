//
//  cassette.c
//  Play a tape into a running bbc emulator, and record back onto it.
//
//    cassette blank.uef
//    cassette -ro program.uef
//
//  While it is connected, r rewinds and q ejects when stdin is a terminal.
//

#include "cassette.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void Usage(void) {
  fprintf(stderr,
          "usage: cassette [-host addr] [-port n] [-ro] tape.uef\n"
          "       Connects to a running bbc emulator and inserts the tape.\n"
          "       A missing file starts as a blank tape and is created when\n"
          "       the guest records. Guest recordings replace the file when\n"
          "       the cassette motor stops. The default address is\n"
          "       127.0.0.1:%d. On a terminal, r rewinds and q ejects.\n",
          CASSETTE_PORT);
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
  if (size < 0 || (unsigned long)size > CASSETTE_MAX_IMAGE) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  buf = NULL;
  if (size > 0) {
    buf = (uint8_t*)malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
      free(buf);
      fclose(fp);
      return -1;
    }
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

// Waits for the emulator or a key. Returns the socket's poll events, with
// *key set to the key or -1. Returns -1 when the wait fails or the
// terminal closes.
static int Wait(int fd, int tty, int* key) {
  struct pollfd pfd[2];
  *key = -1;
  pfd[0].fd = fd;
  pfd[0].events = POLLIN;
  pfd[0].revents = 0;
#ifdef _WIN32
  // WSAPoll takes only sockets, so the console is read between short waits.
  if (SocketPoll(pfd, 1, tty ? 100 : -1) < 0) {
    return -1;
  }
  if (tty) {
    uint8_t ch;
    if (HostStdinByte(&ch)) {
      *key = ch;
    }
  }
#else
  {
    int nfds = 1;
    if (tty) {
      pfd[1].fd = 0;
      pfd[1].events = POLLIN;
      pfd[1].revents = 0;
      nfds = 2;
    }
    while (poll(pfd, (nfds_t)nfds, -1) < 0) {
      if (errno != EINTR) {
        return -1;
      }
    }
    if (tty && (pfd[1].revents & POLLIN) != 0) {
      char ch = 0;
      if (read(0, &ch, 1) <= 0) {
        return -1;
      }
      *key = (unsigned char)ch;
    }
  }
#endif
  return pfd[0].revents;
}

static int WriteBack(const char* path, const uint8_t* data, size_t length) {
  FILE* fp = fopen(path, "wb");
  if (fp == NULL) {
    return -1;
  }
  if (length > 0 && fwrite(data, 1, length, fp) != length) {
    fclose(fp);
    return -1;
  }
  if (fflush(fp) != 0) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

int main(int argc, char** argv) {
  const char* host = "127.0.0.1";
  const char* path = NULL;
  int port = CASSETTE_PORT;
  int protect = 0;
  int fd;
  int code = CASSETTE_ERR;
  int tty;
  uint8_t* image = NULL;
  size_t length = 0;
  char reply[513];
  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-host") == 0) {
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
  if (ReadWholeFile(path, &image, &length) != 0) {
    if (protect) {
      fprintf(stderr, "Unable to read %s\n", path);
      return 1;
    }
    image = NULL;
    length = 0;
  } else if (access(path, W_OK) != 0) {
    protect = 1;
  }
  fd = ConnectTo(host, port);
  if (fd < 0) {
    fprintf(stderr, "Unable to connect to %s:%d\n", host, port);
    free(image);
    return 1;
  }
  if (CassetteSendInsert(fd, protect, BaseName(path), image, length) != 0 ||
      CassetteReadReply(fd, &code, reply, sizeof(reply)) != 0) {
    fprintf(stderr, "The emulator did not accept %s\n", path);
    SocketClose(fd);
    free(image);
    return 1;
  }
  free(image);
  if (code != CASSETTE_OK) {
    fprintf(stderr, "Refused: %s\n", reply[0] != '\0' ? reply : "tape not inserted");
    SocketClose(fd);
    return 1;
  }
  fprintf(stderr, "Tape: %s%s\n", path, protect ? " (read only)" : "");
  tty = isatty(0);
  if (tty) {
    fprintf(stderr, "r rewinds, q ejects\n");
  }
  for (;;) {
    int key;
    int events = Wait(fd, tty, &key);
    if (events < 0) {
      break;
    }
    if (key == 'r' || key == 'R') {
      unsigned char rewind = CASSETTE_REWIND;
      if (CassetteSendAll(fd, &rewind, 1) != 0) {
        break;
      }
      fprintf(stderr, "Rewound\n");
    } else if (key == 'q' || key == 'Q') {
      break;
    }
    if ((events & (POLLIN | POLLHUP | POLLERR)) == 0) {
      continue;
    }
    {
      uint8_t* written = NULL;
      size_t written_len = 0;
      if (CassetteReadImage(fd, &written, &written_len) != 0) {
        break;
      }
      if (!protect && WriteBack(path, written, written_len) != 0) {
        fprintf(stderr, "Unable to update %s\n", path);
        free(written);
        break;
      }
      free(written);
      if (!protect) {
        fprintf(stderr, "Recorded %s\n", path);
      }
    }
  }
  SocketClose(fd);
  fprintf(stderr, "Tape ejected\n");
  return 0;
}
