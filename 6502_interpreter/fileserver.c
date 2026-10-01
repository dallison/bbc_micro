//
//  fileserver.c
//  An Econet file server for the bbc emulator.
//
//  This is not a second BBC. It joins the emulator's virtual Econet wire
//  and answers the Acorn file server protocol, so a machine whose current
//  filing system is NFS or ANFS can catalogue, load, and save here.
//
//    fileserver ~/econet
//    fileserver -station 254 -port 8179 ~/econet
//
//  On the BBC: ./build/bbc -econet 1
//  Then select the network filing system and *I AM yourname.
//  The directory you name is the disc root ($). A subdirectory with the
//  user's name, when it exists, is that user's home. Library is $.LIBRARY
//  when that directory exists. A file called passwd in the root, lines of
//  "NAME SECRET", requires those passwords. With no passwd file, any *I AM
//  is accepted.
//
//  Load and execution addresses are kept in a sibling NAME.inf file, three
//  hex fields: load, exec, access. Those .inf files are not part of the
//  catalogue. Access bits, low to high, are public read, public write,
//  owner read, owner write, locked, directory.
//

#include "bbc_platform.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define ECONET_GROUP "239.255.19.82"
#define ECONET_PORT 8179
#define ECONET_MAX_FRAME 2048
#define FS_COMMAND_PORT 0x99
#define FS_DATA_PORT 0x90
#define FS_BLOCK 256
#define MAX_HANDLES 16
#define MAX_FILES 8
#define NAME_LEN 10

static int g_fd = -1;
static uint32_t g_instance = 0;
static int g_udp_port = ECONET_PORT;
static uint8_t g_station = 254;
static uint8_t g_scout_ctrl = 0x80;
static char g_root[512];
static char g_disc[17];

static struct {
  bool used;
  char path[512];
} g_dir[MAX_HANDLES];

static struct {
  bool used;
  bool write;
  FILE* fp;
  char path[512];
  uint32_t ptr;
  uint32_t length;
} g_file[MAX_FILES];

static bool g_logged = false;
static uint8_t g_client = 0;
static int g_urd = 0;
static int g_csd = 0;
static int g_lib = 0;
static char g_user[NAME_LEN + 1];

static bool g_saving = false;
static uint8_t g_save_client = 0;
static uint8_t g_save_reply = 0;
static uint8_t g_save_ack = 0;
static uint32_t g_save_size = 0;
static uint32_t g_save_got = 0;
static uint32_t g_save_load = 0;
static uint32_t g_save_exec = 0;
static char g_save_path[512];
static FILE* g_save_fp = NULL;

static uint8_t g_stash[ECONET_MAX_FRAME];
static int g_stash_len = 0;

static void Usage(void) {
  fprintf(stderr,
          "usage: fileserver [-station 1-254] [-port n] directory\n"
          "       Serves that directory as an Econet file server disc.\n"
          "       The default station is 254, which is the file server a\n"
          "       BBC looks for. The default wire is UDP port %d, the same\n"
          "       one as bbc -econet. The BBC needs an NFS or ANFS ROM,\n"
          "       then the network filing system, then *I AM name.\n",
          ECONET_PORT);
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

static int ParseStation(const char* text) {
  char* end = NULL;
  long value;
  if (text == NULL || text[0] == '\0') {
    return -1;
  }
  value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 1 || value > 254) {
    return -1;
  }
  return (int)value;
}

static void Upper(char* text) {
  for (; *text != '\0'; text++) {
    *text = (char)toupper((unsigned char)*text);
  }
}

static bool EndsWithInf(const char* name) {
  size_t n = strlen(name);
  return n > 4 && strcmp(name + n - 4, ".inf") == 0;
}

static bool BbcName(const char* name) {
  size_t n = strlen(name);
  size_t i;
  if (n < 1 || n > NAME_LEN) {
    return false;
  }
  for (i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)name[i];
    if (isalnum(ch) || ch == '!' || ch == '-' || ch == '_') {
      continue;
    }
    return false;
  }
  return true;
}

static void DiscName(const char* root) {
  const char* slash = strrchr(root, '/');
  const char* base = (slash != NULL && slash[1] != '\0') ? slash + 1 : root;
  size_t n = strlen(base);
  if (n > 16) {
    n = 16;
  }
  memcpy(g_disc, base, n);
  g_disc[n] = '\0';
  Upper(g_disc);
  if (g_disc[0] == '\0') {
    memcpy(g_disc, "DISC", 5);
  }
}

static bool UnderRoot(const char* path) {
  size_t n = strlen(g_root);
  if (strncmp(path, g_root, n) != 0) {
    return false;
  }
  return path[n] == '\0' || path[n] == '/';
}

static bool JoinName(char* out, size_t cap, const char* dir, const char* name) {
  int wrote;
  if (strcmp(dir, g_root) == 0) {
    wrote = snprintf(out, cap, "%s/%s", g_root, name);
  } else {
    wrote = snprintf(out, cap, "%s/%s", dir, name);
  }
  return wrote > 0 && (size_t)wrote < cap && UnderRoot(out);
}

static int DirHandle(const char* path) {
  int i;
  int free_slot = -1;
  for (i = 0; i < MAX_HANDLES; i++) {
    if (!g_dir[i].used) {
      if (free_slot < 0) {
        free_slot = i;
      }
      continue;
    }
    if (strcmp(g_dir[i].path, path) == 0) {
      return i + 1;
    }
  }
  if (free_slot < 0) {
    return 0;
  }
  g_dir[free_slot].used = true;
  snprintf(g_dir[free_slot].path, sizeof(g_dir[free_slot].path), "%s", path);
  return free_slot + 1;
}

static const char* DirPath(int handle) {
  if (handle < 1 || handle > MAX_HANDLES || !g_dir[handle - 1].used) {
    return NULL;
  }
  return g_dir[handle - 1].path;
}

static void CloseFiles(void) {
  int i;
  for (i = 0; i < MAX_FILES; i++) {
    if (!g_file[i].used) {
      continue;
    }
    if (g_file[i].fp != NULL) {
      fclose(g_file[i].fp);
    }
    g_file[i].used = false;
    g_file[i].fp = NULL;
  }
}

static int FileSlot(int handle) {
  int index;
  if (handle <= 0) {
    return -1;
  }
  // NFS treats the value from OPEN as a bit mask: bit 0 is the first channel.
  if ((handle & (handle - 1)) == 0) {
    for (index = 0; index < MAX_FILES; index++) {
      if (handle == (1 << index) && g_file[index].used) {
        return index;
      }
    }
  }
  if (handle >= 0x40) {
    index = handle - 0x40;
    if (index >= 0 && index < MAX_FILES && g_file[index].used) {
      return index;
    }
  }
  return -1;
}

static bool IsDir(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool IsFile(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static uint32_t FileLength(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    return 0;
  }
  if ((uint64_t)st.st_size > 0xffffffu) {
    return 0xffffffu;
  }
  return (uint32_t)st.st_size;
}

static void Today(uint8_t date[2]) {
  time_t now = time(NULL);
  struct tm local;
  int year;
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  year = local.tm_year + 1900 - 1981;
  if (year < 0) {
    year = 0;
  }
  if (year > 15) {
    year = 15;
  }
  date[0] = (uint8_t)local.tm_mday;
  date[1] = (uint8_t)((year << 4) | ((local.tm_mon + 1) & 0x0f));
}

static void InfPath(const char* path, char* out, size_t cap) {
  snprintf(out, cap, "%s.inf", path);
}

static void ReadInf(const char* path, bool directory, uint32_t* load, uint32_t* exec,
                    uint8_t* access) {
  char inf[540];
  FILE* fp;
  unsigned int a = 0;
  unsigned int b = 0;
  unsigned int c = 0;
  *load = 0;
  *exec = 0;
  *access = directory ? 0x2d : 0x0d;
  InfPath(path, inf, sizeof(inf));
  fp = fopen(inf, "r");
  if (fp == NULL) {
    return;
  }
  if (fscanf(fp, "%x %x %x", &a, &b, &c) >= 2) {
    *load = a;
    *exec = b;
    if (c != 0) {
      *access = (uint8_t)c;
    }
  }
  fclose(fp);
  if (directory) {
    *access = (uint8_t)(*access | 0x20);
  }
}

static void WriteInf(const char* path, uint32_t load, uint32_t exec, uint8_t access) {
  char inf[540];
  FILE* fp;
  InfPath(path, inf, sizeof(inf));
  fp = fopen(inf, "w");
  if (fp == NULL) {
    return;
  }
  fprintf(fp, "%08X %08X %02X\n", load, exec, access);
  fclose(fp);
}

static int BootOption(const char* dir) {
  char path[540];
  FILE* fp;
  int option = 0;
  snprintf(path, sizeof(path), "%s/.bootopt", dir);
  fp = fopen(path, "r");
  if (fp == NULL) {
    return 0;
  }
  if (fscanf(fp, "%d", &option) != 1) {
    option = 0;
  }
  fclose(fp);
  if (option < 0 || option > 7) {
    option = 0;
  }
  return option;
}

static void SetBootOption(const char* dir, int option) {
  char path[540];
  FILE* fp;
  snprintf(path, sizeof(path), "%s/.bootopt", dir);
  fp = fopen(path, "w");
  if (fp == NULL) {
    return;
  }
  fprintf(fp, "%d\n", option & 7);
  fclose(fp);
}

static bool FindChild(const char* dir, const char* name, char* out, size_t cap) {
  DIR* dp;
  struct dirent* ent;
  dp = opendir(dir);
  if (dp == NULL) {
    return false;
  }
  while ((ent = readdir(dp)) != NULL) {
    char upper[NAME_LEN + 1];
    size_t n = strlen(ent->d_name);
    if (n > NAME_LEN) {
      continue;
    }
    char found[NAME_LEN + 1];
    memcpy(found, ent->d_name, n + 1);
    memcpy(upper, found, n + 1);
    Upper(upper);
    if (strcmp(upper, name) != 0) {
      continue;
    }
    closedir(dp);
    return JoinName(out, cap, dir, found);
  }
  closedir(dp);
  return false;
}

// bbc_path is the text inside a file server packet, without its CR.
// A relative name is inside csd. $, @, and & start again from the disc
// root, the current directory, and the library. ^ is the parent.
static bool Resolve(int csd, const char* bbc_path, char* out, size_t cap) {
  const char* base = DirPath(csd);
  char current[512];
  char token[NAME_LEN + 1];
  size_t i = 0;
  if (base == NULL) {
    base = g_root;
  }
  snprintf(current, sizeof(current), "%s", base);
  if (bbc_path[0] == '$' || bbc_path[0] == '@' || bbc_path[0] == '&') {
    if (bbc_path[0] == '$') {
      snprintf(current, sizeof(current), "%s", g_root);
    } else if (bbc_path[0] == '&') {
      base = DirPath(g_lib);
      snprintf(current, sizeof(current), "%s", base != NULL ? base : g_root);
    }
    i = 1;
    if (bbc_path[i] == '.') {
      i++;
    }
  }
  while (bbc_path[i] != '\0') {
    size_t n = 0;
    char child[512];
    while (bbc_path[i] != '\0' && bbc_path[i] != '.') {
      if (n >= NAME_LEN) {
        return false;
      }
      token[n++] = (char)toupper((unsigned char)bbc_path[i]);
      i++;
    }
    token[n] = '\0';
    if (bbc_path[i] == '.') {
      i++;
    }
    if (n == 0) {
      continue;
    }
    if (strcmp(token, "^") == 0) {
      char* slash;
      if (!UnderRoot(current) || strcmp(current, g_root) == 0) {
        return false;
      }
      slash = strrchr(current, '/');
      if (slash == NULL || slash == current) {
        return false;
      }
      *slash = '\0';
      if (!UnderRoot(current)) {
        return false;
      }
      continue;
    }
    if (!BbcName(token)) {
      return false;
    }
    if (!FindChild(current, token, child, sizeof(child))) {
      if (!JoinName(child, sizeof(child), current, token)) {
        return false;
      }
    }
    snprintf(current, sizeof(current), "%s", child);
  }
  if (!UnderRoot(current)) {
    return false;
  }
  snprintf(out, cap, "%s", current);
  return true;
}

static const char* Leaf(const char* path) {
  const char* slash = strrchr(path, '/');
  if (slash == NULL || slash[1] == '\0') {
    return path;
  }
  return slash + 1;
}

static void Pad(uint8_t* dest, const char* text, int width) {
  int i;
  int n = (text != NULL) ? (int)strlen(text) : 0;
  for (i = 0; i < width; i++) {
    dest[i] = (i < n) ? (uint8_t)text[i] : ' ';
  }
}

static void AccessText(uint8_t access, char* out) {
  int n = 0;
  if ((access & 0x20) != 0) {
    out[n++] = 'D';
  }
  if ((access & 0x10) != 0) {
    out[n++] = 'L';
  }
  if ((access & 0x08) != 0) {
    out[n++] = 'W';
  }
  if ((access & 0x04) != 0) {
    out[n++] = 'R';
  }
  out[n++] = '/';
  if ((access & 0x02) != 0) {
    out[n++] = 'W';
  }
  if ((access & 0x01) != 0) {
    out[n++] = 'R';
  }
  out[n] = '\0';
}

static bool PasswordOk(const char* user, const char* secret) {
  char path[540];
  FILE* fp;
  char line[128];
  bool saw = false;
  snprintf(path, sizeof(path), "%s/passwd", g_root);
  fp = fopen(path, "r");
  if (fp == NULL) {
    return true;
  }
  while (fgets(line, sizeof(line), fp) != NULL) {
    char name[64];
    char pass[64];
    int got = sscanf(line, "%63s %63s", name, pass);
    if (got < 1 || name[0] == '#') {
      continue;
    }
    saw = true;
    Upper(name);
    if (strcmp(name, user) != 0) {
      continue;
    }
    fclose(fp);
    if (got < 2) {
      return secret[0] == '\0';
    }
    Upper(pass);
    return strcmp(pass, secret) == 0;
  }
  fclose(fp);
  return !saw;
}

static void Put24(uint8_t* dest, uint32_t value) {
  dest[0] = (uint8_t)value;
  dest[1] = (uint8_t)(value >> 8);
  dest[2] = (uint8_t)(value >> 16);
}

static void Put32(uint8_t* dest, uint32_t value) {
  dest[0] = (uint8_t)value;
  dest[1] = (uint8_t)(value >> 8);
  dest[2] = (uint8_t)(value >> 16);
  dest[3] = (uint8_t)(value >> 24);
}

static uint32_t Get24(const uint8_t* src) {
  return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16);
}

static uint32_t Get32(const uint8_t* src) {
  return Get24(src) | ((uint32_t)src[3] << 24);
}

static int CrText(const uint8_t* src, int avail, char* out, size_t cap) {
  int i;
  size_t n = 0;
  for (i = 0; i < avail && src[i] != 0x0d; i++) {
    if (n + 1 < cap) {
      out[n++] = (char)src[i];
    }
  }
  out[n] = '\0';
  return i;
}

static int OpenSocket(void) {
  int fd;
  int on = 1;
  struct sockaddr_in addr;
  struct ip_mreq mreq;
  if (!SocketStartup()) {
    return -1;
  }
  fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof(on));
#endif
  {
    unsigned char loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
  }
  memset(&addr, 0, sizeof(addr));
#ifdef __APPLE__
  addr.sin_len = sizeof(addr);
#endif
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)g_udp_port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    SocketClose(fd);
    return -1;
  }
  memset(&mreq, 0, sizeof(mreq));
  inet_pton(AF_INET, ECONET_GROUP, &mreq.imr_multiaddr);
#ifdef __APPLE__
  // Same loopback wire as the emulator. A group joined on the Wi-Fi
  // interface never delivers a frame to this program.
  inet_pton(AF_INET, "127.0.0.1", &mreq.imr_interface);
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&mreq.imr_interface,
             sizeof(mreq.imr_interface));
#else
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
#endif
  if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) != 0) {
    SocketClose(fd);
    return -1;
  }
  SocketSetNonBlocking(fd);
  return fd;
}

static void SendRaw(const uint8_t* frame, int len) {
  uint8_t packet[14 + ECONET_MAX_FRAME];
  struct sockaddr_in dest;
  if (len <= 0 || len > ECONET_MAX_FRAME) {
    return;
  }
  memcpy(packet, "ECONET01", 8);
  packet[8] = (uint8_t)(g_instance >> 24);
  packet[9] = (uint8_t)(g_instance >> 16);
  packet[10] = (uint8_t)(g_instance >> 8);
  packet[11] = (uint8_t)g_instance;
  packet[12] = (uint8_t)(len >> 8);
  packet[13] = (uint8_t)len;
  memcpy(packet + 14, frame, (size_t)len);
  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_port = htons((uint16_t)g_udp_port);
  inet_pton(AF_INET, ECONET_GROUP, &dest.sin_addr);
  sendto(g_fd, (const char*)packet, 14 + len, 0, (struct sockaddr*)&dest, sizeof(dest));
}

static int MakeDir(const char* path) {
#ifdef _WIN32
  return _mkdir(path);
#else
  return mkdir(path, 0755);
#endif
}

static int RecvRaw(uint8_t* frame, int cap, int timeout_ms, bool use_stash) {
  struct pollfd pfd;
  if (use_stash && g_stash_len > 0) {
    int n = g_stash_len < cap ? g_stash_len : cap;
    memcpy(frame, g_stash, (size_t)n);
    g_stash_len = 0;
    return n;
  }
  pfd.fd = g_fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  if (SocketPoll(&pfd, 1, timeout_ms) <= 0) {
    return 0;
  }
  for (;;) {
    uint8_t buf[14 + ECONET_MAX_FRAME];
    ssize_t n = recvfrom(g_fd, (char*)buf, (int)sizeof(buf), 0, NULL, NULL);
    uint32_t instance;
    int len;
    if (n < 0) {
      return 0;
    }
    if (n < 14 || memcmp(buf, "ECONET01", 8) != 0) {
      continue;
    }
    instance = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) | ((uint32_t)buf[10] << 8) |
               (uint32_t)buf[11];
    len = ((int)buf[12] << 8) | buf[13];
    if (instance == g_instance || len <= 0 || len > cap || 14 + len > (int)n) {
      continue;
    }
    memcpy(frame, buf + 14, (size_t)len);
    return len;
  }
}

static void Stash(const uint8_t* frame, int len) {
  if (g_stash_len != 0 || len <= 0 || len > ECONET_MAX_FRAME) {
    return;
  }
  memcpy(g_stash, frame, (size_t)len);
  g_stash_len = len;
}

static void SendAck(uint8_t to, uint8_t net) {
  uint8_t frame[4];
  frame[0] = to;
  frame[1] = net;
  frame[2] = g_station;
  frame[3] = 0;
  SendRaw(frame, 4);
}

static bool WaitAck(uint8_t from, int timeout_ms) {
  int left = timeout_ms;
  while (left > 0) {
    uint8_t frame[ECONET_MAX_FRAME];
    int step = left > 50 ? 50 : left;
    int n = RecvRaw(frame, (int)sizeof(frame), step, false);
    left -= step;
    if (n <= 0) {
      continue;
    }
    if (n == 4 && frame[0] == g_station && frame[2] == from) {
      return true;
    }
    if (n >= 4 && frame[0] == g_station) {
      Stash(frame, n);
    }
  }
  return false;
}

static bool Transmit(uint8_t to, uint8_t port, const uint8_t* payload, int len) {
  int attempt;
  for (attempt = 0; attempt < 6; attempt++) {
    uint8_t scout[6];
    uint8_t data[ECONET_MAX_FRAME];
    scout[0] = to;
    scout[1] = 0;
    scout[2] = g_station;
    scout[3] = 0;
    scout[4] = g_scout_ctrl;
    scout[5] = port;
    SendRaw(scout, 6);
    if (!WaitAck(to, 200)) {
      continue;
    }
    if (4 + len > ECONET_MAX_FRAME) {
      return false;
    }
    data[0] = to;
    data[1] = 0;
    data[2] = g_station;
    data[3] = 0;
    if (len > 0) {
      memcpy(data + 4, payload, (size_t)len);
    }
    SendRaw(data, 4 + len);
    if (WaitAck(to, 400)) {
      return true;
    }
  }
  return false;
}

static void Reply(uint8_t to, uint8_t port, const uint8_t* body, int len) {
  Transmit(to, port, body, len);
}

static void Fail(uint8_t to, uint8_t port, uint8_t code, const char* message) {
  uint8_t body[128];
  size_t n = strlen(message);
  if (n > 100) {
    n = 100;
  }
  body[0] = 0;
  body[1] = code;
  memcpy(body + 2, message, n);
  body[2 + n] = 0x0d;
  Reply(to, port, body, (int)(3 + n));
}

static bool NeedLogin(uint8_t to, uint8_t port) {
  if (g_logged && g_client == to) {
    return true;
  }
  Fail(to, port, 0xbf, "Who are you?");
  return false;
}

static void CmdIAm(uint8_t from, uint8_t port, const char* args) {
  char user[64];
  char secret[64];
  char home[512];
  char library[512];
  const char* rest = args;
  int urd;
  int got;
  uint8_t body[8];
  while (*rest == ' ') {
    rest++;
  }
  got = sscanf(rest, "%63s %63s", user, secret);
  if (got < 1) {
    Fail(from, port, 0xfe, "Bad command");
    return;
  }
  if (got < 2) {
    secret[0] = '\0';
  }
  Upper(user);
  Upper(secret);
  if (!PasswordOk(user, secret)) {
    Fail(from, port, 0xbb, "Wrong password");
    return;
  }
  CloseFiles();
  memset(g_dir, 0, sizeof(g_dir));
  if (!FindChild(g_root, user, home, sizeof(home)) || !IsDir(home)) {
    snprintf(home, sizeof(home), "%s", g_root);
  }
  urd = DirHandle(home);
  g_urd = urd;
  g_csd = urd;
  g_lib = urd;
  if (FindChild(g_root, "LIBRARY", library, sizeof(library)) && IsDir(library)) {
    g_lib = DirHandle(library);
  }
  g_logged = true;
  g_client = from;
  snprintf(g_user, sizeof(g_user), "%s", user);
  body[0] = 5;
  body[1] = 0;
  body[2] = (uint8_t)g_urd;
  body[3] = (uint8_t)g_csd;
  body[4] = (uint8_t)g_lib;
  body[5] = (uint8_t)BootOption(home);
  fprintf(stderr, "station %u: I AM %s\n", from, user);
  Reply(from, port, body, 6);
}

static void CmdCat(uint8_t from, uint8_t port, int csd, const char* args) {
  char path[512];
  char leaf[NAME_LEN + 1];
  uint8_t body[32];
  size_t n;
  const char* name = args;
  while (*name == ' ') {
    name++;
  }
  if (name[0] == '\0') {
    const char* cur = DirPath(csd);
    if (cur == NULL || !Resolve(csd, "", path, sizeof(path))) {
      snprintf(path, sizeof(path), "%s", cur != NULL ? cur : g_root);
    }
  } else if (!Resolve(csd, name, path, sizeof(path)) || !IsDir(path)) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  snprintf(leaf, sizeof(leaf), "%s", strcmp(path, g_root) == 0 ? "$" : Leaf(path));
  Upper(leaf);
  body[0] = 3;
  body[1] = 0;
  n = strlen(leaf);
  memcpy(body + 2, leaf, n);
  body[2 + n] = 0x0d;
  Reply(from, port, body, (int)(3 + n));
}

static void CmdDir(uint8_t from, uint8_t port, int csd, const char* args, bool library) {
  char path[512];
  uint8_t body[4];
  const char* name = args;
  int handle;
  while (*name == ' ') {
    name++;
  }
  if (name[0] == '\0') {
    handle = library ? g_urd : g_urd;
    if (!library && g_urd == 0) {
      Fail(from, port, 0xbf, "Who are you?");
      return;
    }
  } else if (!Resolve(csd, name, path, sizeof(path)) || !IsDir(path)) {
    Fail(from, port, 0xd6, "Not found");
    return;
  } else {
    handle = DirHandle(path);
  }
  if (handle == 0) {
    Fail(from, port, 0x64, "Too many directories");
    return;
  }
  if (library) {
    g_lib = handle;
    body[0] = 9;
  } else {
    g_csd = handle;
    body[0] = 7;
  }
  body[1] = 0;
  body[2] = (uint8_t)handle;
  Reply(from, port, body, 3);
}

static void CmdDelete(uint8_t from, uint8_t port, int csd, const char* args) {
  char path[512];
  char inf[540];
  uint32_t load = 0;
  uint32_t exec = 0;
  uint8_t access = 0;
  uint8_t body[16];
  const char* name = args;
  while (*name == ' ') {
    name++;
  }
  if (!Resolve(csd, name, path, sizeof(path)) || strcmp(path, g_root) == 0) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  if (!IsFile(path) && !IsDir(path)) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  ReadInf(path, IsDir(path), &load, &exec, &access);
  if ((access & 0x10) != 0) {
    Fail(from, port, 0xbd, "Locked");
    return;
  }
  if (IsDir(path)) {
    Fail(from, port, 0xaf, "Types don't match");
    return;
  }
  if (remove(path) != 0) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  InfPath(path, inf, sizeof(inf));
  remove(inf);
  body[0] = 0;
  body[1] = 0;
  Reply(from, port, body, 2);
}

static void CmdCdir(uint8_t from, uint8_t port, int csd, const char* args) {
  char path[512];
  uint8_t body[2];
  const char* name = args;
  while (*name == ' ') {
    name++;
  }
  if (!Resolve(csd, name, path, sizeof(path))) {
    Fail(from, port, 0xfd, "Bad name");
    return;
  }
  if (IsFile(path) || IsDir(path)) {
    Fail(from, port, 0xaf, "Already exists");
    return;
  }
  if (MakeDir(path) != 0) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  WriteInf(path, 0, 0, 0x2d);
  body[0] = 0;
  body[1] = 0;
  Reply(from, port, body, 2);
}

static void CmdInfo(uint8_t from, uint8_t port, int csd, const char* args) {
  char path[512];
  char text[80];
  char access[12];
  uint32_t load;
  uint32_t exec;
  uint8_t attr;
  uint8_t body[96];
  size_t n;
  const char* name = args;
  while (*name == ' ') {
    name++;
  }
  if (!Resolve(csd, name, path, sizeof(path)) || (!IsFile(path) && !IsDir(path))) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  ReadInf(path, IsDir(path), &load, &exec, &attr);
  AccessText(attr, access);
  snprintf(text, sizeof(text), "%-10s %-8s %08X %08X %06X", Leaf(path), access, load, exec,
           IsDir(path) ? 0 : FileLength(path));
  Upper(text);
  n = strlen(text);
  body[0] = 4;
  body[1] = 0;
  memcpy(body + 2, text, n);
  body[2 + n] = 0x80;
  Reply(from, port, body, (int)(3 + n));
}

static uint8_t ParseAccess(const char* text, uint8_t previous) {
  uint8_t access = previous & 0x20;
  const char* slash = strchr(text, '/');
  const char* p;
  if (slash == NULL) {
    slash = text + strlen(text);
  }
  for (p = text; p < slash; p++) {
    char ch = (char)toupper((unsigned char)*p);
    if (ch == 'R') {
      access |= 0x04;
    } else if (ch == 'W') {
      access |= 0x08;
    } else if (ch == 'L') {
      access |= 0x10;
    }
  }
  for (p = slash; *p != '\0'; p++) {
    char ch = (char)toupper((unsigned char)*p);
    if (ch == 'R') {
      access |= 0x01;
    } else if (ch == 'W') {
      access |= 0x02;
    }
  }
  return access;
}

static void CmdAccess(uint8_t from, uint8_t port, int csd, const char* args) {
  char name[64];
  char rights[32];
  char path[512];
  uint32_t load;
  uint32_t exec;
  uint8_t access;
  uint8_t body[2];
  int got = sscanf(args, "%63s %31s", name, rights);
  if (got < 2) {
    Fail(from, port, 0xfe, "Bad command");
    return;
  }
  Upper(name);
  if (!Resolve(csd, name, path, sizeof(path)) || (!IsFile(path) && !IsDir(path))) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  ReadInf(path, IsDir(path), &load, &exec, &access);
  access = ParseAccess(rights, access);
  WriteInf(path, load, exec, access);
  body[0] = 0;
  body[1] = 0;
  Reply(from, port, body, 2);
}

static bool CommandWord(const char* line, const char* word) {
  size_t n = strlen(word);
  size_t i;
  for (i = 0; i < n; i++) {
    if ((char)toupper((unsigned char)line[i]) != word[i]) {
      return false;
    }
  }
  return line[n] == '\0' || line[n] == ' ' || line[n] == '.';
}

static const char* CommandRest(const char* line) {
  while (*line != '\0' && *line != ' ') {
    if (*line == '.') {
      line++;
      break;
    }
    line++;
  }
  while (*line == ' ') {
    line++;
  }
  return line;
}

static bool IAmCommand(const char* line, const char** args) {
  if (strncmp(line, "I AM", 4) == 0) {
    *args = line + 4;
  } else if (strncmp(line, "I A.", 4) == 0) {
    *args = line + 4;
  } else if (strncmp(line, "IAM", 3) == 0) {
    *args = line + 3;
  } else {
    return false;
  }
  while (**args == ' ') {
    (*args)++;
  }
  return true;
}

static void Command(uint8_t from, const uint8_t* payload, int len) {
  char line[256];
  int csd;
  uint8_t reply = payload[0];
  const char* rest;
  const char* args = NULL;
  if (len < 6) {
    return;
  }
  csd = payload[3];
  CrText(payload + 5, len - 5, line, sizeof(line));
  Upper(line);
  rest = line;
  while (*rest == ' ') {
    rest++;
  }
  if (IAmCommand(rest, &args)) {
    CmdIAm(from, reply, args);
    return;
  }
  if (rest[0] == '.' && (rest[1] == '\0' || rest[1] == ' ')) {
    if (!NeedLogin(from, reply)) {
      return;
    }
    CmdCat(from, reply, csd, rest + 1);
    return;
  }
  if (!NeedLogin(from, reply)) {
    return;
  }
  if (CommandWord(rest, "CAT")) {
    CmdCat(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "DIR")) {
    CmdDir(from, reply, csd, CommandRest(rest), false);
  } else if (CommandWord(rest, "LIB") || (rest[0] == 'L' && rest[1] == 'I')) {
    CmdDir(from, reply, csd, CommandRest(rest), true);
  } else if (CommandWord(rest, "DELETE") || CommandWord(rest, "DESTROY") ||
             (rest[0] == 'D' && rest[1] == '.')) {
    CmdDelete(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "CDIR") || (rest[0] == 'C' && rest[1] == 'D')) {
    CmdCdir(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "INFO") || (rest[0] == 'I' && rest[1] == '.')) {
    CmdInfo(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "EX") || CommandWord(rest, "EXAMINE")) {
    CmdInfo(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "ACCESS") || (rest[0] == 'A' && (rest[1] == '.' || rest[1] == '\0' || rest[1] == ' '))) {
    CmdAccess(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "BYE") || CommandWord(rest, "LOGOFF") ||
             (rest[0] == 'B' && rest[1] == 'Y')) {
    uint8_t body[2] = {0, 0};
    CloseFiles();
    g_logged = false;
    Reply(from, reply, body, 2);
  } else {
    Fail(from, reply, 0xfe, "Bad command");
  }
}

struct Entry {
  char name[NAME_LEN + 1];
  uint32_t load;
  uint32_t exec;
  uint32_t length;
  uint8_t access;
};

static int Collect(const char* dir, struct Entry* entries, int cap) {
  DIR* dp;
  struct dirent* ent;
  int count = 0;
  dp = opendir(dir);
  if (dp == NULL) {
    return 0;
  }
  while ((ent = readdir(dp)) != NULL && count < cap) {
    char path[512];
    char name[NAME_LEN + 1];
    size_t n = strlen(ent->d_name);
    int at;
    if (ent->d_name[0] == '.' || EndsWithInf(ent->d_name) || n > NAME_LEN) {
      continue;
    }
    memcpy(name, ent->d_name, n + 1);
    Upper(name);
    if (!BbcName(name) || !JoinName(path, sizeof(path), dir, ent->d_name)) {
      continue;
    }
    if (!IsFile(path) && !IsDir(path)) {
      continue;
    }
    at = count;
    while (at > 0 && strcmp(entries[at - 1].name, name) > 0) {
      entries[at] = entries[at - 1];
      at--;
    }
    snprintf(entries[at].name, sizeof(entries[at].name), "%s", name);
    ReadInf(path, IsDir(path), &entries[at].load, &entries[at].exec, &entries[at].access);
    entries[at].length = IsDir(path) ? 0 : FileLength(path);
    count++;
  }
  closedir(dp);
  return count;
}

static void Examine(uint8_t from, const uint8_t* payload, int len) {
  char name[256];
  char path[512];
  struct Entry entries[128];
  uint8_t body[4096];
  int total;
  int start;
  int want;
  int got;
  int arg;
  int i;
  int used;
  uint8_t reply = payload[0];
  int csd = len > 3 ? payload[3] : g_csd;
  if (!NeedLogin(from, reply) || len < 8) {
    return;
  }
  arg = payload[5];
  start = payload[6];
  want = payload[7];
  CrText(payload + 8, len - 8, name, sizeof(name));
  if (name[0] == '\0') {
    const char* cur = DirPath(csd);
    snprintf(path, sizeof(path), "%s", cur != NULL ? cur : g_root);
  } else if (!Resolve(csd, name, path, sizeof(path)) || !IsDir(path)) {
    Fail(from, reply, 0xd6, "Not found");
    return;
  }
  total = Collect(path, entries, 128);
  if (start > total) {
    start = total;
  }
  if (want == 0 || start + want > total) {
    want = total - start;
  }
  if (want > 80) {
    want = 80;
  }
  body[0] = 0;
  body[1] = 0;
  body[2] = (uint8_t)want;
  body[3] = 0;
  used = 4;
  for (i = 0; i < want; i++) {
    struct Entry* e = &entries[start + i];
    uint8_t date[2];
    char access[12];
    char column[20];
    Today(date);
    if (arg == 0 && used + 27 < (int)sizeof(body)) {
      memset(body + used, ' ', 10);
      memcpy(body + used, e->name, strlen(e->name));
      Put32(body + used + 10, e->load);
      Put32(body + used + 14, e->exec);
      body[used + 18] = e->access;
      body[used + 19] = date[0];
      body[used + 20] = date[1];
      body[used + 21] = 0;
      body[used + 22] = 0;
      body[used + 23] = 0;
      Put24(body + used + 24, e->length);
      used += 27;
    } else if (arg == 2 && used + 11 < (int)sizeof(body)) {
      body[used] = NAME_LEN;
      Pad(body + used + 1, e->name, NAME_LEN);
      used += 11;
    } else if (arg == 3 && used + 22 < (int)sizeof(body)) {
      AccessText(e->access, access);
      snprintf(column, sizeof(column), "%-10s %-8s", e->name, access);
      Pad(body + used, column, 19);
      body[used + 19] = ' ';
      body[used + 20] = 0x00;
      used += 21;
    } else if (used + 40 < (int)sizeof(body)) {
      AccessText(e->access, access);
      snprintf(column, sizeof(column), "%-10s %s", e->name, access);
      memcpy(body + used, column, strlen(column));
      used += (int)strlen(column);
      body[used++] = 0x00;
    }
  }
  // A zero byte is printed as a new line. &80 ends the list, so the last
  // name still gets its return before the prompt.
  body[used++] = 0x80;
  got = used;
  Reply(from, reply, body, got);
}

static void CatalogueHeader(uint8_t from, const uint8_t* payload, int len) {
  char name[256];
  char path[512];
  uint8_t body[40];
  uint8_t reply = payload[0];
  int csd = len > 3 ? payload[3] : g_csd;
  const char* leaf;
  if (!NeedLogin(from, reply)) {
    return;
  }
  CrText(payload + 5, len > 5 ? len - 5 : 0, name, sizeof(name));
  if (name[0] == '\0') {
    const char* cur = DirPath(csd);
    snprintf(path, sizeof(path), "%s", cur != NULL ? cur : g_root);
  } else if (!Resolve(csd, name, path, sizeof(path)) || !IsDir(path)) {
    Fail(from, reply, 0xd6, "Not found");
    return;
  }
  leaf = strcmp(path, g_root) == 0 ? "$" : Leaf(path);
  body[0] = 0;
  body[1] = 0;
  Pad(body + 2, leaf, NAME_LEN);
  memcpy(body + 12, "OWN", 3);
  body[15] = ' ';
  Pad(body + 16, g_disc, 13);
  body[29] = 0x0d;
  body[30] = 0x80;
  Reply(from, reply, body, 31);
}

static void FinishSave(uint8_t from) {
  uint8_t date[2];
  uint8_t body[8];
  uint8_t access = 0x0d;
  if (g_save_fp != NULL) {
    fclose(g_save_fp);
    g_save_fp = NULL;
  }
  WriteInf(g_save_path, g_save_load, g_save_exec, access);
  Today(date);
  body[0] = 0;
  body[1] = 0;
  body[2] = access;
  body[3] = date[0];
  body[4] = date[1];
  g_saving = false;
  Reply(from, g_save_reply, body, 5);
}

static void SaveData(uint8_t from, const uint8_t* data, int len) {
  uint8_t ack[1] = {0};
  int store;
  if (!g_saving || from != g_save_client) {
    return;
  }
  if (g_save_got >= g_save_size) {
    FinishSave(from);
    return;
  }
  store = len;
  if ((uint32_t)store > g_save_size - g_save_got) {
    store = (int)(g_save_size - g_save_got);
  }
  if (store > 0 && g_save_fp != NULL) {
    fwrite(data, 1, (size_t)store, g_save_fp);
    g_save_got += (uint32_t)store;
  }
  if (g_save_got >= g_save_size) {
    FinishSave(from);
    return;
  }
  Reply(from, g_save_ack, ack, 1);
}

static void Save(uint8_t from, const uint8_t* payload, int len) {
  char name[256];
  char path[512];
  uint8_t body[8];
  uint8_t reply = payload[0];
  int csd;
  if (!NeedLogin(from, reply) || len < 17) {
    return;
  }
  csd = payload[3];
  CrText(payload + 16, len - 16, name, sizeof(name));
  if (name[0] == '\0' || !Resolve(csd, name, path, sizeof(path)) || IsDir(path)) {
    Fail(from, reply, name[0] == '\0' ? 0xfd : 0xd6, name[0] == '\0' ? "Bad name" : "Not found");
    return;
  }
  if (IsFile(path)) {
    uint32_t old_load;
    uint32_t old_exec;
    uint8_t old_access;
    ReadInf(path, false, &old_load, &old_exec, &old_access);
    if ((old_access & 0x10) != 0) {
      Fail(from, reply, 0xbd, "Locked");
      return;
    }
  }
  if (g_save_fp != NULL) {
    fclose(g_save_fp);
    g_save_fp = NULL;
  }
  g_save_fp = fopen(path, "wb");
  if (g_save_fp == NULL) {
    Fail(from, reply, 0xd6, "Not found");
    return;
  }
  g_saving = true;
  g_save_client = from;
  g_save_reply = reply;
  g_save_ack = payload[2];
  g_save_load = Get32(payload + 5);
  g_save_exec = Get32(payload + 9);
  g_save_size = Get24(payload + 13);
  g_save_got = 0;
  snprintf(g_save_path, sizeof(g_save_path), "%s", path);
  fprintf(stderr, "station %u: SAVE %s (%u bytes)\n", from, name, g_save_size);
  body[0] = 0;
  body[1] = 0;
  body[2] = FS_DATA_PORT;
  body[3] = (uint8_t)FS_BLOCK;
  body[4] = (uint8_t)(FS_BLOCK >> 8);
  Reply(from, reply, body, 5);
  if (g_save_size == 0) {
    FinishSave(from);
  }
}

// Deliver `count` bytes on the data port. The first `valid` bytes come from
// the file at `offset`; the rest are padding for a read that ran past the end.
static bool SendSpan(uint8_t from, uint8_t port, FILE* fp, uint32_t offset, uint32_t valid,
                     uint32_t count) {
  uint8_t block[FS_BLOCK];
  uint32_t sent = 0;
  if (valid > 0 && (fflush(fp) != 0 || fseek(fp, (long)offset, SEEK_SET) != 0)) {
    return false;
  }
  while (sent < count) {
    uint32_t chunk = FS_BLOCK;
    uint32_t file_bytes = 0;
    if (chunk > count - sent) {
      chunk = count - sent;
    }
    memset(block, 0, (size_t)chunk);
    if (sent < valid) {
      file_bytes = valid - sent;
      if (file_bytes > chunk) {
        file_bytes = chunk;
      }
      if (fread(block, 1, (size_t)file_bytes, fp) != (size_t)file_bytes) {
        memset(block, 0, (size_t)chunk);
      }
    }
    if (!Transmit(from, port, block, (int)chunk)) {
      return false;
    }
    sent += chunk;
  }
  return true;
}

static void SendFile(uint8_t from, uint8_t data_port, const char* path, uint32_t length) {
  FILE* fp = fopen(path, "rb");
  uint8_t block[FS_BLOCK];
  uint32_t sent = 0;
  if (fp == NULL) {
    return;
  }
  while (sent < length) {
    size_t chunk = FS_BLOCK;
    size_t got;
    if (chunk > length - sent) {
      chunk = length - sent;
    }
    got = fread(block, 1, chunk, fp);
    if (got == 0) {
      break;
    }
    if (!Transmit(from, data_port, block, (int)got)) {
      break;
    }
    sent += (uint32_t)got;
  }
  fclose(fp);
}

static void Load(uint8_t from, const uint8_t* payload, int len, bool library_too) {
  char name[256];
  char path[512];
  uint32_t load;
  uint32_t exec;
  uint32_t length;
  uint8_t access;
  uint8_t date[2];
  uint8_t body[64];
  uint8_t reply;
  uint8_t data_port;
  int csd;
  int used;
  size_t name_n;
  if (len < 6) {
    return;
  }
  reply = payload[0];
  if (!NeedLogin(from, reply)) {
    return;
  }
  data_port = payload[2];
  csd = payload[3];
  CrText(payload + 5, len - 5, name, sizeof(name));
  if (!Resolve(csd, name, path, sizeof(path)) || !IsFile(path)) {
    if (library_too && g_lib != 0 && Resolve(g_lib, name, path, sizeof(path)) && IsFile(path)) {
      /* found in the library */
    } else {
      Fail(from, reply, library_too ? 0xfe : 0xd6, library_too ? "Bad command" : "Not found");
      return;
    }
  }
  ReadInf(path, false, &load, &exec, &access);
  length = FileLength(path);
  Today(date);
  fprintf(stderr, "station %u: LOAD %s\n", from, name);
  body[0] = 0;
  body[1] = 0;
  Put32(body + 2, load);
  Put32(body + 6, exec);
  Put24(body + 10, length);
  body[13] = access;
  body[14] = date[0];
  body[15] = date[1];
  name_n = strlen(name);
  if (name_n > 30) {
    name_n = 30;
  }
  memcpy(body + 16, name, name_n);
  body[16 + name_n] = 0x0d;
  Reply(from, reply, body, (int)(17 + name_n));
  if (length > 0) {
    SendFile(from, data_port, path, length);
  }
  used = 2;
  body[0] = 0;
  body[1] = 0;
  Reply(from, reply, body, used);
}

static void Open(uint8_t from, const uint8_t* payload, int len) {
  char name[256];
  char path[512];
  uint8_t body[4];
  uint8_t reply = payload[0];
  int csd;
  int i;
  bool create;
  bool read_only;
  if (!NeedLogin(from, reply) || len < 8) {
    return;
  }
  csd = payload[3];
  create = payload[5] == 0;
  read_only = payload[6] != 0;
  CrText(payload + 7, len - 7, name, sizeof(name));
  if (!Resolve(csd, name, path, sizeof(path))) {
    Fail(from, reply, 0xfd, "Bad name");
    return;
  }
  if (IsDir(path)) {
    Fail(from, reply, 0xaf, "Types don't match");
    return;
  }
  if (!IsFile(path)) {
    if (!create || read_only) {
      Fail(from, reply, 0xd6, "Not found");
      return;
    }
  }
  for (i = 0; i < MAX_FILES; i++) {
    if (!g_file[i].used) {
      break;
    }
  }
  if (i == MAX_FILES) {
    Fail(from, reply, 0x64, "Too many open files");
    return;
  }
  g_file[i].fp = fopen(path, read_only ? "rb" : "r+b");
  if (g_file[i].fp == NULL && !read_only) {
    g_file[i].fp = fopen(path, "w+b");
  }
  if (g_file[i].fp == NULL) {
    Fail(from, reply, 0xd6, "Not found");
    return;
  }
  g_file[i].used = true;
  g_file[i].write = !read_only;
  g_file[i].ptr = 0;
  g_file[i].length = FileLength(path);
  snprintf(g_file[i].path, sizeof(g_file[i].path), "%s", path);
  body[0] = 0;
  body[1] = 0;
  // Bit 0 is the first open file. NFS turns that mask into a channel number.
  body[2] = (uint8_t)(1u << i);
  Reply(from, reply, body, 3);
}

static void Close(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[2] = {0, 0};
  uint8_t reply = payload[0];
  int handle;
  int slot;
  if (!NeedLogin(from, reply) || len < 6) {
    return;
  }
  handle = payload[5];
  if (handle == 0) {
    CloseFiles();
    Reply(from, reply, body, 2);
    return;
  }
  slot = FileSlot(handle);
  if (slot < 0) {
    Fail(from, reply, 0x65, "Not open");
    return;
  }
  fclose(g_file[slot].fp);
  g_file[slot].fp = NULL;
  g_file[slot].used = false;
  Reply(from, reply, body, 2);
}

static void GetByte(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[4];
  uint8_t reply = payload[0];
  int slot;
  int ch;
  if (!NeedLogin(from, reply) || len < 6) {
    return;
  }
  slot = FileSlot(payload[5]);
  if (slot < 0) {
    Fail(from, reply, 0x65, "Not open");
    return;
  }
  if (fseek(g_file[slot].fp, (long)g_file[slot].ptr, SEEK_SET) != 0) {
    Fail(from, reply, 0x73, "Beyond file end");
    return;
  }
  ch = fgetc(g_file[slot].fp);
  body[0] = 0;
  body[1] = 0;
  if (ch == EOF) {
    body[2] = 0xfe;
    body[3] = 0xc0;
  } else {
    g_file[slot].ptr++;
    body[2] = (uint8_t)ch;
    body[3] = g_file[slot].ptr >= g_file[slot].length ? 0x80 : 0x00;
  }
  Reply(from, reply, body, 4);
}

static void PutByte(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[2] = {0, 0};
  uint8_t reply = payload[0];
  int slot;
  if (!NeedLogin(from, reply) || len < 7) {
    return;
  }
  slot = FileSlot(payload[5]);
  if (slot < 0 || !g_file[slot].write) {
    Fail(from, reply, 0x65, "Not open");
    return;
  }
  if (fseek(g_file[slot].fp, (long)g_file[slot].ptr, SEEK_SET) != 0 ||
      fputc(payload[6], g_file[slot].fp) == EOF) {
    Fail(from, reply, 0x73, "Beyond file end");
    return;
  }
  g_file[slot].ptr++;
  if (g_file[slot].ptr > g_file[slot].length) {
    g_file[slot].length = g_file[slot].ptr;
  }
  Reply(from, reply, body, 2);
}

static void ReadPointer(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[6];
  uint8_t reply = payload[0];
  int slot;
  uint32_t value;
  if (!NeedLogin(from, reply) || len < 7) {
    return;
  }
  slot = FileSlot(payload[5]);
  if (slot < 0) {
    Fail(from, reply, 0x65, "Not open");
    return;
  }
  value = payload[6] == 0 ? g_file[slot].ptr : g_file[slot].length;
  body[0] = 0;
  body[1] = 0;
  Put24(body + 2, value);
  Reply(from, reply, body, 5);
}

static void SetPointer(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[2] = {0, 0};
  uint8_t reply = payload[0];
  int slot;
  if (!NeedLogin(from, reply) || len < 10) {
    return;
  }
  slot = FileSlot(payload[5]);
  if (slot < 0) {
    Fail(from, reply, 0x65, "Not open");
    return;
  }
  if (payload[6] == 0) {
    g_file[slot].ptr = Get24(payload + 7);
  }
  Reply(from, reply, body, 2);
}

static void ReadEof(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[3];
  uint8_t reply = payload[0];
  int slot;
  if (!NeedLogin(from, reply) || len < 6) {
    return;
  }
  slot = FileSlot(payload[5]);
  if (slot < 0) {
    Fail(from, reply, 0x65, "Not open");
    return;
  }
  body[0] = 0;
  body[1] = 0;
  body[2] = g_file[slot].ptr >= g_file[slot].length ? 0xff : 0x00;
  Reply(from, reply, body, 3);
}

static void ObjectInfo(uint8_t from, const uint8_t* payload, int len) {
  char name[256];
  char path[512];
  uint8_t body[24];
  uint8_t reply = payload[0];
  int arg;
  int csd;
  int kind = 0;
  uint32_t load = 0;
  uint32_t exec = 0;
  uint32_t length = 0;
  uint8_t access = 0;
  uint8_t date[2];
  int used = 3;
  if (!NeedLogin(from, reply) || len < 7) {
    return;
  }
  arg = payload[5];
  csd = payload[3];
  CrText(payload + 6, len - 6, name, sizeof(name));
  if (Resolve(csd, name, path, sizeof(path))) {
    if (IsDir(path)) {
      kind = 2;
    } else if (IsFile(path)) {
      kind = 1;
    }
  }
  if (kind != 0) {
    ReadInf(path, kind == 2, &load, &exec, &access);
    length = kind == 2 ? 0 : FileLength(path);
  }
  Today(date);
  body[0] = 0;
  body[1] = 0;
  body[2] = (uint8_t)kind;
  if (arg == 2) {
    Put32(body + 3, load);
    Put32(body + 7, exec);
    used = 11;
  } else if (arg == 3) {
    Put24(body + 3, length);
    used = 6;
  } else if (arg == 4) {
    body[3] = access;
    body[4] = 0;
    used = 5;
  } else if (arg == 5) {
    Put32(body + 3, load);
    Put32(body + 7, exec);
    Put24(body + 11, length);
    body[14] = access;
    body[15] = date[0];
    body[16] = date[1];
    body[17] = 0;
    used = 18;
  } else if (arg == 1) {
    body[3] = date[0];
    body[4] = date[1];
    used = 5;
  } else if (arg == 6) {
    const char* leaf = kind == 0 ? "" : (strcmp(path, g_root) == 0 ? "$" : Leaf(path));
    body[2] = 0;
    body[3] = 0;
    body[4] = NAME_LEN;
    Pad(body + 5, leaf, NAME_LEN);
    body[15] = 0;
    body[16] = 0;
    used = 17;
  }
  Reply(from, reply, body, used);
}

static void DeleteObject(uint8_t from, const uint8_t* payload, int len) {
  char name[256];
  if (len < 6) {
    return;
  }
  CrText(payload + 5, len - 5, name, sizeof(name));
  CmdDelete(from, payload[0], payload[3], name);
}

static void Environment(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[40];
  uint8_t reply = len > 0 ? payload[0] : 0;
  const char* csd = DirPath(g_csd);
  const char* lib = DirPath(g_lib);
  const char* csd_leaf = (csd != NULL && strcmp(csd, g_root) != 0) ? Leaf(csd) : "$";
  const char* lib_leaf = (lib != NULL && strcmp(lib, g_root) != 0) ? Leaf(lib) : "$";
  if (!NeedLogin(from, reply)) {
    return;
  }
  body[0] = 0;
  body[1] = 0;
  body[2] = (uint8_t)strlen(g_disc);
  Pad(body + 3, g_disc, 16);
  Pad(body + 19, csd_leaf, NAME_LEN);
  Pad(body + 29, lib_leaf, NAME_LEN);
  Reply(from, reply, body, 39);
}

static void Version(uint8_t from, uint8_t reply) {
  const char* text = "Host FS 1.0";
  uint8_t body[32];
  size_t n = strlen(text);
  body[0] = 0;
  body[1] = 0;
  memcpy(body + 2, text, n);
  body[2 + n] = 0x0d;
  Reply(from, reply, body, (int)(3 + n));
}

// *TYPE reads with OSBGET. NFS sends that on the data port, one byte at a
// time, and the scout's control byte carries the sequence bit in bit 0.
// The reply is written over the four-byte request: return code, data, flag.
static void ByteStream(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[4];
  uint8_t reply;
  uint8_t function;
  int slot;
  int ch;
  if (len < 3) {
    return;
  }
  reply = payload[0];
  function = payload[1];
  if (!NeedLogin(from, reply)) {
    return;
  }
  slot = FileSlot(payload[2]);
  body[0] = 0;
  body[1] = 0;
  body[2] = 0;
  body[3] = 0;
  if (slot < 0) {
    body[3] = 0xc0;
    Reply(from, reply, body, 4);
    return;
  }
  if (function == 9) {
    if (len < 4 || !g_file[slot].write ||
        fseek(g_file[slot].fp, (long)g_file[slot].ptr, SEEK_SET) != 0 ||
        fputc(payload[3], g_file[slot].fp) == EOF) {
      body[3] = 0xc0;
      Reply(from, reply, body, 4);
      return;
    }
    g_file[slot].ptr++;
    if (g_file[slot].ptr > g_file[slot].length) {
      g_file[slot].length = g_file[slot].ptr;
    }
    Reply(from, reply, body, 4);
    return;
  }
  if (fseek(g_file[slot].fp, (long)g_file[slot].ptr, SEEK_SET) != 0) {
    body[2] = 0xfe;
    body[3] = 0xc0;
    Reply(from, reply, body, 4);
    return;
  }
  ch = fgetc(g_file[slot].fp);
  if (ch == EOF) {
    body[2] = 0xfe;
    body[3] = 0xc0;
  } else {
    g_file[slot].ptr++;
    body[2] = (uint8_t)ch;
    body[3] = g_file[slot].ptr >= g_file[slot].length ? 0x80 : 0x00;
  }
  Reply(from, reply, body, 4);
}

// OSGBPB. NFS 3.34 sends function 10 as:
// reply port, function, data port, CSD, LIB, handle mask, flag,
// 24-bit count, 24-bit offset. Flag 0 reads from the sequential pointer
// and advances it. Any other flag reads at the supplied offset and leaves
// the pointer where it was. The client has already reserved `count` bytes,
// so a read past the end still sends that many (the spare ones are padding)
// and the completion word says how many of them belonged to the file.
// Bit 7 of that word is set when the transfer reached the end of the file.
static void GetBytes(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[8];
  uint8_t reply;
  uint8_t data_port;
  uint32_t count;
  uint32_t offset;
  uint32_t valid;
  uint32_t length;
  int slot;
  bool sequential;
  if (len < 13) {
    return;
  }
  reply = payload[0];
  if (!NeedLogin(from, reply)) {
    return;
  }
  data_port = payload[2];
  slot = FileSlot(payload[5]);
  if (slot < 0) {
    Fail(from, reply, 0x65, "Not open");
    return;
  }
  sequential = payload[6] == 0;
  count = Get24(payload + 7);
  length = g_file[slot].length;
  offset = sequential ? g_file[slot].ptr : Get24(payload + 10);
  if (offset >= length) {
    valid = 0;
  } else if (count > length - offset) {
    valid = length - offset;
  } else {
    valid = count;
  }
  if (valid > 0 &&
      (fflush(g_file[slot].fp) != 0 || fseek(g_file[slot].fp, (long)offset, SEEK_SET) != 0)) {
    Fail(from, reply, 0x73, "Beyond file end");
    return;
  }
  body[0] = 0;
  body[1] = 0;
  Reply(from, reply, body, 2);
  if (count > 0 && !SendSpan(from, data_port, g_file[slot].fp, offset, valid, count)) {
    return;
  }
  if (sequential) {
    g_file[slot].ptr = offset + valid;
  }
  body[0] = 0;
  body[1] = 0;
  body[2] = (valid < count || offset + valid >= length) ? 0x80 : 0x00;
  Put24(body + 3, valid);
  body[6] = 0;
  Reply(from, reply, body, 7);
}

static void Dispatch(uint8_t from, uint8_t scout_port, const uint8_t* payload, int len) {
  uint8_t function;
  if (scout_port == FS_DATA_PORT) {
    if (!g_saving && len >= 2 && (payload[1] == 8 || payload[1] == 9)) {
      ByteStream(from, payload, len);
    } else {
      SaveData(from, payload, len);
    }
    return;
  }
  if (scout_port != FS_COMMAND_PORT || len < 2) {
    return;
  }
  function = payload[1];
  // OSBGET/OSBPUT are four bytes (reply port, function, channel mask, data),
  // not the six-byte header used by the other calls.
  if ((function == 8 || function == 9) && len < 6) {
    ByteStream(from, payload, len);
    return;
  }
  if (function == 0) {
    Command(from, payload, len);
  } else if (function == 1) {
    Save(from, payload, len);
  } else if (function == 2) {
    Load(from, payload, len, false);
  } else if (function == 3) {
    Examine(from, payload, len);
  } else if (function == 4) {
    CatalogueHeader(from, payload, len);
  } else if (function == 5) {
    Load(from, payload, len, true);
  } else if (function == 6) {
    Open(from, payload, len);
  } else if (function == 7) {
    Close(from, payload, len);
  } else if (function == 8) {
    GetByte(from, payload, len);
  } else if (function == 9) {
    PutByte(from, payload, len);
  } else if (function == 10) {
    GetBytes(from, payload, len);
  } else if (function == 12) {
    ReadPointer(from, payload, len);
  } else if (function == 13) {
    SetPointer(from, payload, len);
  } else if (function == 17) {
    ReadEof(from, payload, len);
  } else if (function == 18) {
    ObjectInfo(from, payload, len);
  } else if (function == 20) {
    DeleteObject(from, payload, len);
  } else if (function == 21) {
    Environment(from, payload, len);
  } else if (function == 22) {
    uint8_t body[2] = {0, 0};
    const char* home = DirPath(g_urd);
    if (!NeedLogin(from, payload[0])) {
      return;
    }
    if (home != NULL && len > 5) {
      SetBootOption(home, payload[5] & 7);
    }
    Reply(from, payload[0], body, 2);
  } else if (function == 23) {
    uint8_t body[2] = {0, 0};
    CloseFiles();
    g_logged = false;
    Reply(from, payload[0], body, 2);
  } else if (function == 25) {
    Version(from, payload[0]);
  } else {
    Fail(from, payload[0], 0x85, "Invalid function");
  }
}

static bool Listening(uint8_t port) {
  return port == FS_COMMAND_PORT || port == FS_DATA_PORT;
}

static void OnFrame(const uint8_t* frame, int len) {
  static uint8_t scout_from = 0;
  static uint8_t scout_port = 0;
  static bool scout_open = false;
  if (len < 4 || frame[0] != g_station) {
    return;
  }
  if (len == 4) {
    return;
  }
  // Bit 7 marks a scout. Bit 0 is the byte-stream sequence number, which the
  // reply scout has to repeat or the client retries until it gives up.
  if (len == 6 && (frame[4] & 0xfe) == 0x80 && Listening(frame[5]) &&
      !(g_saving && scout_open && frame[2] == g_save_client && scout_port == FS_DATA_PORT)) {
    SendAck(frame[2], frame[3]);
    scout_from = frame[2];
    scout_port = frame[5];
    scout_open = true;
    g_scout_ctrl = (uint8_t)(0x80 | (frame[4] & 0x01));
    return;
  }
  if (len >= 5 && frame[2] == scout_from && scout_open) {
    SendAck(frame[2], frame[3]);
    scout_open = false;
    Dispatch(frame[2], scout_port, frame + 4, len - 4);
  }
}

static void Serve(void) {
  fprintf(stderr, "File server station %u, UDP port %d, disc %s\n", g_station, g_udp_port, g_root);
  fprintf(stderr, "On the BBC: -econet 1, select NFS, then *I AM name\n");
  for (;;) {
    uint8_t frame[ECONET_MAX_FRAME];
    int n = RecvRaw(frame, (int)sizeof(frame), 200, true);
    if (n > 0) {
      OnFrame(frame, n);
    }
  }
}

int main(int argc, char** argv) {
  int i;
  const char* dir = NULL;
  char resolved[1024];
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-station") == 0) {
      int station;
      if (i + 1 >= argc || (station = ParseStation(argv[++i])) < 0) {
        Usage();
        return 1;
      }
      g_station = (uint8_t)station;
    } else if (strcmp(argv[i], "-port") == 0) {
      if (i + 1 >= argc || (g_udp_port = ParsePort(argv[++i])) < 0) {
        Usage();
        return 1;
      }
    } else if (argv[i][0] == '-') {
      Usage();
      return 1;
    } else if (dir != NULL) {
      Usage();
      return 1;
    } else {
      dir = argv[i];
    }
  }
  if (dir == NULL || realpath(dir, resolved) == NULL || !IsDir(resolved) ||
      strlen(resolved) >= sizeof(g_root)) {
    Usage();
    return 1;
  }
  snprintf(g_root, sizeof(g_root), "%s", resolved);
  DiscName(g_root);
  g_fd = OpenSocket();
  if (g_fd < 0) {
    fprintf(stderr, "fileserver: cannot join the Econet wire on port %d\n", g_udp_port);
    return 1;
  }
  g_instance = (HostProcessId() << 10) | 0x3ffu;
  if (g_instance == 0) {
    g_instance = 1;
  }
  Serve();
  return 0;
}
