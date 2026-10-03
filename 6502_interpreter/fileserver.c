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
//  "NAME SECRET" or "NAME SECRET S", requires those passwords. S marks a
//  system user, who may *NEWUSER and *PRIV. "-" is an empty password. With
//  no passwd file, any *I AM is accepted and that user may create accounts.
//
//  Load and execution addresses are kept in a sibling NAME.inf file, three
//  hex fields: load, exec, access. Those .inf files are not part of the
//  catalogue. Access bits, low to high, are public read, public write,
//  owner read, owner write, locked, directory.
//
//  An ELF executable is left untouched on disk. Its load and execution
//  addresses come from the program headers, and a load or a read builds
//  the BBC memory image from those headers at the moment it is asked for.
//
//  ADFS uses '.' between directory names. A UNIX path is shown the other
//  way round: '/' becomes '.', and a '.' inside a name becomes ','.
//  src/main.c is $.SRC.MAIN,C. A name from the BBC is read back the same way.
//
//  A * command whose first word is a Mac or Unix program, or a symbolic
//  link to one, runs that program on this machine. The current directory
//  is the working directory. The rest of the line is the argument list,
//  spelled the same way as the catalogue: '.' is '/' and ',' is '.'.
//  Command words match in either case. Arguments keep the case the BBC
//  sent. A 6502 file, including a 6502 ELF, is still loaded by the BBC.
//  Windows keeps the load-and-run reply for every name.
//
//  Output that fits in one reply is printed by the filing system. Longer
//  output is written to $.HOST.Ostation and $.HOST.Rstation is run to
//  print it. When the text fits between BASIC's variables and BASIC's
//  stack, that program reads it in one transfer. Otherwise it reads
//  pieces of that free memory. With no such gap it reads 256 bytes at a
//  time. The text file is removed after printing.
//

#include "bbc_platform.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#endif

#define ECONET_GROUP "239.255.19.82"
#define ECONET_PORT 8179
#define ECONET_MAX_FRAME 2048
#define FS_COMMAND_PORT 0x99
#define FS_DATA_PORT 0x90
#define FS_BLOCK 256
// NFS copies the first two bytes of the PutBytes reply into its block size,
// and the second of those bytes is also the return code. The size therefore
// has to fit in one byte, with a zero return code beside it.
#define FS_PUT_BLOCK 128
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

static bool g_putting = false;
static uint8_t g_put_client = 0;
static uint8_t g_put_reply = 0;
static uint8_t g_put_port = 0;
static int g_put_slot = -1;
static bool g_put_sequential = false;
static uint32_t g_put_offset = 0;
static uint32_t g_put_count = 0;
static uint32_t g_put_got = 0;
static uint32_t g_put_valid = 0;

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

// A directory entry the BBC may see. '.' is a UNIX dot that will be shown
// as ',', and ',' is already that ADFS form. A leading dot stays hidden.
static bool FileName(const char* name) {
  size_t n = strlen(name);
  size_t i;
  if (n < 1 || n > NAME_LEN || name[0] == '.') {
    return false;
  }
  for (i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)name[i];
    if (isalnum(ch) || ch == '!' || ch == '-' || ch == '_' || ch == '.' || ch == ',') {
      continue;
    }
    return false;
  }
  return true;
}

static void UnixToAdfs(char* name) {
  for (; *name != '\0'; name++) {
    if (*name == '.') {
      *name = ',';
    }
  }
}

static void AdfsToUnix(char* name) {
  for (; *name != '\0'; name++) {
    if (*name == ',') {
      *name = '.';
    }
  }
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
  UnixToAdfs(g_disc);
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

#define ELF_MAX_FILE (8u * 1024u * 1024u)
#define ELF_MAX_IMAGE (256u * 1024u)
#define ELF_PT_LOAD 1

static uint16_t ElfU16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t ElfU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ElfU64(const uint8_t* p) {
  return (uint64_t)ElfU32(p) | ((uint64_t)ElfU32(p + 4) << 32);
}

// bytes, when not NULL, receives a freshly allocated memory image. The file
// on disk is not rewritten. A file that is not a loadable ELF returns false.
static bool ElfImage(const char* path, uint32_t* load, uint32_t* exec, uint32_t* length,
                     uint8_t** bytes) {
  FILE* fp;
  uint8_t ident[16];
  uint8_t* file = NULL;
  long file_size;
  size_t n;
  bool is64;
  uint64_t entry;
  uint64_t phoff;
  unsigned phentsize;
  unsigned phnum;
  unsigned i;
  bool saw = false;
  uint64_t base = 0;
  uint64_t end = 0;
  struct {
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
  } seg[64];
  unsigned nseg = 0;
  uint8_t* image;
  if (bytes != NULL) {
    *bytes = NULL;
  }
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return false;
  }
  if (fread(ident, 1, 16, fp) != 16 || ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' ||
      ident[3] != 'F' || ident[5] != 1 || (ident[4] != 1 && ident[4] != 2)) {
    fclose(fp);
    return false;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return false;
  }
  file_size = ftell(fp);
  if (file_size < 16 || (unsigned long)file_size > ELF_MAX_FILE) {
    fclose(fp);
    return false;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return false;
  }
  file = (uint8_t*)malloc((size_t)file_size);
  if (file == NULL) {
    fclose(fp);
    return false;
  }
  n = fread(file, 1, (size_t)file_size, fp);
  fclose(fp);
  if (n != (size_t)file_size) {
    free(file);
    return false;
  }
  is64 = file[4] == 2;
  if (is64) {
    if (file_size < 64) {
      free(file);
      return false;
    }
    entry = ElfU64(file + 24);
    phoff = ElfU64(file + 32);
    phentsize = ElfU16(file + 54);
    phnum = ElfU16(file + 56);
  } else {
    if (file_size < 52) {
      free(file);
      return false;
    }
    entry = ElfU32(file + 24);
    phoff = ElfU32(file + 28);
    phentsize = ElfU16(file + 42);
    phnum = ElfU16(file + 44);
  }
  if (phnum == 0 || phnum > 64 || phentsize < (is64 ? 56u : 32u) ||
      phoff > (uint64_t)file_size || phoff + (uint64_t)phnum * phentsize > (uint64_t)file_size) {
    free(file);
    return false;
  }
  for (i = 0; i < phnum; i++) {
    const uint8_t* ph = file + phoff + (uint64_t)i * phentsize;
    uint32_t type;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t seg_end;
    if (is64) {
      type = ElfU32(ph);
      offset = ElfU64(ph + 8);
      vaddr = ElfU64(ph + 16);
      filesz = ElfU64(ph + 32);
      memsz = ElfU64(ph + 40);
    } else {
      type = ElfU32(ph);
      offset = ElfU32(ph + 4);
      vaddr = ElfU32(ph + 8);
      filesz = ElfU32(ph + 16);
      memsz = ElfU32(ph + 20);
    }
    if (type != ELF_PT_LOAD || memsz == 0) {
      continue;
    }
    if (filesz > memsz) {
      filesz = memsz;
    }
    if (offset > (uint64_t)file_size || filesz > (uint64_t)file_size - offset ||
        vaddr > 0xffffffffu || memsz > ELF_MAX_IMAGE || vaddr + memsz < vaddr) {
      free(file);
      return false;
    }
    seg_end = vaddr + memsz;
    if (!saw || vaddr < base) {
      base = vaddr;
    }
    if (!saw || seg_end > end) {
      end = seg_end;
    }
    saw = true;
    seg[nseg].offset = offset;
    seg[nseg].vaddr = vaddr;
    seg[nseg].filesz = filesz;
    seg[nseg].memsz = memsz;
    nseg++;
  }
  if (!saw || end <= base || end - base > ELF_MAX_IMAGE || entry > 0xffffffffu) {
    free(file);
    return false;
  }
  *load = (uint32_t)base;
  *exec = (uint32_t)entry;
  *length = (uint32_t)(end - base);
  if (bytes == NULL) {
    free(file);
    return true;
  }
  image = (uint8_t*)calloc(1, (size_t)*length);
  if (image == NULL) {
    free(file);
    return false;
  }
  for (i = 0; i < nseg; i++) {
    if (seg[i].filesz == 0) {
      continue;
    }
    memcpy(image + (size_t)(seg[i].vaddr - base), file + seg[i].offset, (size_t)seg[i].filesz);
  }
  free(file);
  *bytes = image;
  return true;
}

static void DescribeFile(const char* path, bool directory, uint32_t* load, uint32_t* exec,
                         uint32_t* length, uint8_t* access) {
  uint32_t elf_load = 0;
  uint32_t elf_exec = 0;
  uint32_t elf_len = 0;
  ReadInf(path, directory, load, exec, access);
  if (length != NULL) {
    *length = directory ? 0 : FileLength(path);
  }
  if (!directory && ElfImage(path, &elf_load, &elf_exec, &elf_len, NULL)) {
    if (load != NULL) {
      *load = elf_load;
    }
    if (exec != NULL) {
      *exec = elf_exec;
    }
    if (length != NULL) {
      *length = elf_len;
    }
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
// '.' separates directories (a UNIX '/'). ',' in a component is a UNIX '.'.
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
    AdfsToUnix(token);
    if (!FileName(token)) {
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

// The leaf the BBC prints. '$' is the disc root. A UNIX '.' is shown as ','.
static void ShowLeaf(char* out, size_t cap, const char* path) {
  if (path == NULL || strcmp(path, g_root) == 0) {
    snprintf(out, cap, "$");
    return;
  }
  snprintf(out, cap, "%s", Leaf(path));
  Upper(out);
  UnixToAdfs(out);
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

#define MAX_ACCOUNTS 64

struct Account {
  char name[64];
  char secret[64];
  bool system;
};

static int LoadAccounts(struct Account* list, bool* existed) {
  char path[540];
  FILE* fp;
  char line[128];
  int count = 0;
  *existed = false;
  snprintf(path, sizeof(path), "%s/passwd", g_root);
  fp = fopen(path, "r");
  if (fp == NULL) {
    return 0;
  }
  *existed = true;
  while (fgets(line, sizeof(line), fp) != NULL && count < MAX_ACCOUNTS) {
    char name[64];
    char pass[64];
    char priv[16];
    int got = sscanf(line, "%63s %63s %15s", name, pass, priv);
    if (got < 1 || name[0] == '#') {
      continue;
    }
    Upper(name);
    snprintf(list[count].name, sizeof(list[count].name), "%s", name);
    if (got < 2 || strcmp(pass, "-") == 0) {
      list[count].secret[0] = '\0';
    } else {
      Upper(pass);
      snprintf(list[count].secret, sizeof(list[count].secret), "%s", pass);
    }
    list[count].system = got >= 3 && (priv[0] == 'S' || priv[0] == 's');
    count++;
  }
  fclose(fp);
  return count;
}

static bool SaveAccounts(const struct Account* list, int count) {
  char path[540];
  char temp[540];
  FILE* fp;
  int i;
  snprintf(path, sizeof(path), "%s/passwd", g_root);
  snprintf(temp, sizeof(temp), "%s/passwd.tmp", g_root);
  fp = fopen(temp, "w");
  if (fp == NULL) {
    return false;
  }
  for (i = 0; i < count; i++) {
    const char* secret = list[i].secret[0] != '\0' ? list[i].secret : "-";
    if (list[i].system) {
      fprintf(fp, "%s %s S\n", list[i].name, secret);
    } else {
      fprintf(fp, "%s %s\n", list[i].name, secret);
    }
  }
  if (fclose(fp) != 0) {
    remove(temp);
    return false;
  }
  if (rename(temp, path) != 0) {
    remove(temp);
    return false;
  }
  return true;
}

static int FindAccount(const struct Account* list, int count, const char* user) {
  int i;
  for (i = 0; i < count; i++) {
    if (strcmp(list[i].name, user) == 0) {
      return i;
    }
  }
  return -1;
}

static bool CallerIsSystem(const struct Account* list, int count, bool existed) {
  int i;
  int mine;
  bool any = false;
  if (!existed) {
    return true;
  }
  for (i = 0; i < count; i++) {
    if (list[i].system) {
      any = true;
    }
  }
  mine = FindAccount(list, count, g_user);
  if (mine >= 0 && list[mine].system) {
    return true;
  }
  return !any;
}

static bool PasswordOk(const char* user, const char* secret) {
  struct Account list[MAX_ACCOUNTS];
  bool existed = false;
  int count = LoadAccounts(list, &existed);
  int at;
  if (!existed) {
    return true;
  }
  at = FindAccount(list, count, user);
  if (at < 0) {
    return count == 0;
  }
  return strcmp(list[at].secret, secret) == 0;
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
  ShowLeaf(leaf, sizeof(leaf), path);
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

static bool HasWild(const char* name) {
  return strchr(name, '*') != NULL || strchr(name, '#') != NULL;
}

static bool WildMatch(const char* pattern, const char* text) {
  if (*pattern == '*') {
    return WildMatch(pattern + 1, text) || (*text != '\0' && WildMatch(pattern, text + 1));
  }
  if (*pattern == '#') {
    return *text != '\0' && WildMatch(pattern + 1, text + 1);
  }
  if (*pattern == '\0' || *text == '\0') {
    return *pattern == '\0' && *text == '\0';
  }
  if (toupper((unsigned char)*pattern) != toupper((unsigned char)*text)) {
    return false;
  }
  return WildMatch(pattern + 1, text + 1);
}

static bool PatternDir(int csd, const char* spec, char* dir, size_t dir_cap, char* pattern,
                       size_t pattern_cap) {
  char parent[256];
  const char* dot = strrchr(spec, '.');
  const char* leaf = spec;
  parent[0] = '\0';
  if (dot != NULL) {
    size_t n = (size_t)(dot - spec);
    if (n >= sizeof(parent)) {
      return false;
    }
    memcpy(parent, spec, n);
    parent[n] = '\0';
    leaf = dot + 1;
  }
  if (HasWild(parent) || strlen(leaf) >= pattern_cap) {
    return false;
  }
  if (parent[0] == '\0') {
    const char* cur = DirPath(csd);
    snprintf(dir, dir_cap, "%s", cur != NULL ? cur : g_root);
  } else if (!Resolve(csd, parent, dir, dir_cap) || !IsDir(dir)) {
    return false;
  }
  snprintf(pattern, pattern_cap, "%s", leaf);
  return true;
}

static int MatchObjects(const char* dir, const char* pattern, char paths[][512], int cap,
                        bool files_only) {
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
    if (ent->d_name[0] == '.' || EndsWithInf(ent->d_name) || n > NAME_LEN) {
      continue;
    }
    memcpy(name, ent->d_name, n + 1);
    Upper(name);
    if (!FileName(name) || !JoinName(path, sizeof(path), dir, ent->d_name)) {
      continue;
    }
    UnixToAdfs(name);
    if (!WildMatch(pattern, name)) {
      continue;
    }
    if (files_only) {
      if (!IsFile(path)) {
        continue;
      }
    } else if (!IsFile(path) && !IsDir(path)) {
      continue;
    }
    snprintf(paths[count], 512, "%s", path);
    count++;
  }
  closedir(dp);
  return count;
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
  if (HasWild(name)) {
    char dir[512];
    char pattern[NAME_LEN + 1];
    char paths[64][512];
    int count;
    int i;
    if (!PatternDir(csd, name, dir, sizeof(dir), pattern, sizeof(pattern))) {
      Fail(from, port, 0xd6, "Not found");
      return;
    }
    count = MatchObjects(dir, pattern, paths, 64, true);
    if (count == 0) {
      Fail(from, port, 0xd6, "Not found");
      return;
    }
    for (i = 0; i < count; i++) {
      ReadInf(paths[i], false, &load, &exec, &access);
      if ((access & 0x10) != 0) {
        Fail(from, port, 0xbd, "Locked");
        return;
      }
    }
    for (i = 0; i < count; i++) {
      char gone[540];
      remove(paths[i]);
      InfPath(paths[i], gone, sizeof(gone));
      remove(gone);
    }
    body[0] = 0;
    body[1] = 0;
    Reply(from, port, body, 2);
    return;
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

static void RewritePath(char* path, size_t cap, const char* old_path, const char* new_path) {
  size_t n = strlen(old_path);
  char rest[512];
  if (strcmp(path, old_path) == 0) {
    snprintf(path, cap, "%s", new_path);
    return;
  }
  if (strncmp(path, old_path, n) == 0 && path[n] == '/') {
    snprintf(rest, sizeof(rest), "%s", path + n);
    snprintf(path, cap, "%s%s", new_path, rest);
  }
}

static void NoteRename(const char* old_path, const char* new_path) {
  int i;
  for (i = 0; i < MAX_HANDLES; i++) {
    if (g_dir[i].used) {
      RewritePath(g_dir[i].path, sizeof(g_dir[i].path), old_path, new_path);
    }
  }
  for (i = 0; i < MAX_FILES; i++) {
    if (g_file[i].used) {
      RewritePath(g_file[i].path, sizeof(g_file[i].path), old_path, new_path);
    }
  }
  if (g_saving) {
    RewritePath(g_save_path, sizeof(g_save_path), old_path, new_path);
  }
}

static bool NextWord(const char** line, char* word, size_t cap) {
  size_t n = 0;
  const char* p = *line;
  while (*p == ' ') {
    p++;
  }
  if (*p == '\0') {
    word[0] = '\0';
    return false;
  }
  while (*p != '\0' && *p != ' ') {
    if (n + 1 < cap) {
      word[n++] = *p;
    }
    p++;
  }
  word[n] = '\0';
  while (*p == ' ') {
    p++;
  }
  *line = p;
  return true;
}

static void CmdRename(uint8_t from, uint8_t port, int csd, const char* args) {
  char old_name[256];
  char new_name[256];
  char old_path[512];
  char new_path[512];
  char parent[512];
  char old_inf[540];
  char new_inf[540];
  uint32_t load;
  uint32_t exec;
  uint8_t access;
  uint8_t body[2] = {0, 0};
  const char* rest = args;
  const char* slash;
  if (!NextWord(&rest, old_name, sizeof(old_name)) || !NextWord(&rest, new_name, sizeof(new_name))) {
    Fail(from, port, 0xfe, "Bad command");
    return;
  }
  if (!Resolve(csd, old_name, old_path, sizeof(old_path)) || strcmp(old_path, g_root) == 0 ||
      (!IsFile(old_path) && !IsDir(old_path))) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  if (!Resolve(csd, new_name, new_path, sizeof(new_path)) || strcmp(new_path, g_root) == 0) {
    Fail(from, port, 0xfd, "Bad name");
    return;
  }
  slash = strrchr(new_path, '/');
  if (slash == NULL || slash == new_path) {
    Fail(from, port, 0xfd, "Bad name");
    return;
  }
  memcpy(parent, new_path, (size_t)(slash - new_path));
  parent[slash - new_path] = '\0';
  if (!IsDir(parent)) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  if (strcmp(old_path, new_path) == 0) {
    Reply(from, port, body, 2);
    return;
  }
  if (IsDir(old_path) && strncmp(new_path, old_path, strlen(old_path)) == 0 &&
      new_path[strlen(old_path)] == '/') {
    Fail(from, port, 0xfd, "Bad name");
    return;
  }
  if (IsFile(new_path) || IsDir(new_path)) {
    Fail(from, port, 0xaf, "Already exists");
    return;
  }
  ReadInf(old_path, IsDir(old_path), &load, &exec, &access);
  if ((access & 0x10) != 0) {
    Fail(from, port, 0xbd, "Locked");
    return;
  }
  if (rename(old_path, new_path) != 0) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  InfPath(old_path, old_inf, sizeof(old_inf));
  InfPath(new_path, new_inf, sizeof(new_inf));
  if (IsFile(old_inf) || IsDir(old_inf)) {
    rename(old_inf, new_inf);
  }
  NoteRename(old_path, new_path);
  fprintf(stderr, "station %u: RENAME %s %s\n", from, old_name, new_name);
  Reply(from, port, body, 2);
}

static int InfoLine(const char* path, char* text, size_t cap) {
  char access[12];
  char leaf[NAME_LEN + 1];
  uint32_t load;
  uint32_t exec;
  uint8_t attr;
  bool directory = IsDir(path);
  uint32_t length = 0;
  DescribeFile(path, directory, &load, &exec, &length, &attr);
  AccessText(attr, access);
  ShowLeaf(leaf, sizeof(leaf), path);
  snprintf(text, cap, "%-10s %-8s %08X %08X %06X", leaf, access, load, exec, length);
  Upper(text);
  return (int)strlen(text);
}

static void CmdInfo(uint8_t from, uint8_t port, int csd, const char* args) {
  char path[512];
  char text[80];
  uint8_t body[ECONET_MAX_FRAME];
  int n;
  const char* name = args;
  while (*name == ' ') {
    name++;
  }
  body[0] = 4;
  body[1] = 0;
  if (HasWild(name)) {
    char dir[512];
    char pattern[NAME_LEN + 1];
    char paths[64][512];
    int count;
    int i;
    int used = 2;
    if (!PatternDir(csd, name, dir, sizeof(dir), pattern, sizeof(pattern))) {
      Fail(from, port, 0xd6, "Not found");
      return;
    }
    count = MatchObjects(dir, pattern, paths, 64, false);
    if (count == 0) {
      Fail(from, port, 0xd6, "Not found");
      return;
    }
    for (i = 0; i < count; i++) {
      n = InfoLine(paths[i], text, sizeof(text));
      if (used + n + 1 > ECONET_MAX_FRAME - 4) {
        break;
      }
      memcpy(body + used, text, (size_t)n);
      used += n;
      body[used++] = 0x00;
    }
    body[used - 1] = 0x80;
    Reply(from, port, body, used);
    return;
  }
  if (!Resolve(csd, name, path, sizeof(path)) || (!IsFile(path) && !IsDir(path))) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  n = InfoLine(path, text, sizeof(text));
  memcpy(body + 2, text, (size_t)n);
  body[2 + n] = 0x80;
  Reply(from, port, body, 3 + n);
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
  if (HasWild(name)) {
    char dir[512];
    char pattern[NAME_LEN + 1];
    char paths[64][512];
    int count;
    int i;
    if (!PatternDir(csd, name, dir, sizeof(dir), pattern, sizeof(pattern))) {
      Fail(from, port, 0xd6, "Not found");
      return;
    }
    count = MatchObjects(dir, pattern, paths, 64, false);
    if (count == 0) {
      Fail(from, port, 0xd6, "Not found");
      return;
    }
    for (i = 0; i < count; i++) {
      ReadInf(paths[i], IsDir(paths[i]), &load, &exec, &access);
      access = ParseAccess(rights, access);
      WriteInf(paths[i], load, exec, access);
    }
    body[0] = 0;
    body[1] = 0;
    Reply(from, port, body, 2);
    return;
  }
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

static bool Starts(const char* line, const char* word) {
  size_t i;
  for (i = 0; word[i] != '\0'; i++) {
    if ((char)toupper((unsigned char)line[i]) != word[i]) {
      return false;
    }
  }
  return true;
}

static bool Two(const char* line, char a, char b) {
  return (char)toupper((unsigned char)line[0]) == a &&
         (char)toupper((unsigned char)line[1]) == b;
}

static bool IAmCommand(const char* line, const char** args) {
  if (Starts(line, "I AM")) {
    *args = line + 4;
  } else if (Starts(line, "I A.")) {
    *args = line + 4;
  } else if (Starts(line, "IAM")) {
    *args = line + 3;
  } else {
    return false;
  }
  while (**args == ' ') {
    (*args)++;
  }
  return true;
}

static void CmdRun(uint8_t from, uint8_t port, const char* line) {
  uint8_t body[96];
  size_t n = 0;
  while (*line == ' ') {
    line++;
  }
  if (*line == '\0') {
    Fail(from, port, 0xfe, "Bad command");
    return;
  }
  while (line[n] != '\0' && line[n] != ' ' && n < 80) {
    n++;
  }
  body[0] = 8;
  body[1] = 0;
  memcpy(body + 2, line, n);
  body[2 + n] = 0x0d;
  fprintf(stderr, "station %u: RUN %.*s\n", from, (int)n, line);
  Reply(from, port, body, (int)(3 + n));
}

static void CmdPass(uint8_t from, uint8_t port, const char* args) {
  struct Account list[MAX_ACCOUNTS];
  bool existed = false;
  int count = LoadAccounts(list, &existed);
  int at = FindAccount(list, count, g_user);
  char old_secret[64];
  char new_secret[64];
  const char* rest = args;
  uint8_t body[2] = {0, 0};
  const char* current = at >= 0 ? list[at].secret : "";
  if (!NextWord(&rest, old_secret, sizeof(old_secret))) {
    Fail(from, port, 0xfe, "Bad command");
    return;
  }
  Upper(old_secret);
  if (!NextWord(&rest, new_secret, sizeof(new_secret))) {
    if (current[0] != '\0') {
      Fail(from, port, 0xbb, "Wrong password");
      return;
    }
    snprintf(new_secret, sizeof(new_secret), "%s", old_secret);
    old_secret[0] = '\0';
  } else {
    Upper(new_secret);
  }
  if (strcmp(current, old_secret) != 0) {
    Fail(from, port, 0xbb, "Wrong password");
    return;
  }
  if (at < 0) {
    if (count >= MAX_ACCOUNTS) {
      Fail(from, port, 0xc0, "Too many users");
      return;
    }
    at = count++;
    snprintf(list[at].name, sizeof(list[at].name), "%s", g_user);
    list[at].system = true;
  }
  snprintf(list[at].secret, sizeof(list[at].secret), "%s", new_secret);
  if (!SaveAccounts(list, count)) {
    Fail(from, port, 0xc7, "Disc full");
    return;
  }
  Reply(from, port, body, 2);
}

static void CmdNewUser(uint8_t from, uint8_t port, const char* args) {
  struct Account list[MAX_ACCOUNTS];
  bool existed = false;
  int count = LoadAccounts(list, &existed);
  char name[64];
  char secret[64];
  char path[512];
  const char* rest = args;
  uint8_t body[2] = {0, 0};
  if (!CallerIsSystem(list, count, existed)) {
    Fail(from, port, 0xbd, "Insufficient privilege");
    return;
  }
  if (!NextWord(&rest, name, sizeof(name)) || !BbcName(name)) {
    Fail(from, port, 0xfe, "Bad command");
    return;
  }
  Upper(name);
  if (!NextWord(&rest, secret, sizeof(secret))) {
    secret[0] = '\0';
  } else {
    Upper(secret);
  }
  if (FindAccount(list, count, name) >= 0) {
    Fail(from, port, 0xaf, "Already exists");
    return;
  }
  if (count + (FindAccount(list, count, g_user) < 0 ? 1 : 0) >= MAX_ACCOUNTS) {
    Fail(from, port, 0xc0, "Too many users");
    return;
  }
  if (!JoinName(path, sizeof(path), g_root, name)) {
    Fail(from, port, 0xfd, "Bad name");
    return;
  }
  if (IsFile(path)) {
    Fail(from, port, 0xaf, "Types don't match");
    return;
  }
  if (!IsDir(path)) {
    if (MakeDir(path) != 0) {
      Fail(from, port, 0xc7, "Disc full");
      return;
    }
    WriteInf(path, 0, 0, 0x2d);
  }
  if (FindAccount(list, count, g_user) < 0) {
    snprintf(list[count].name, sizeof(list[count].name), "%s", g_user);
    list[count].secret[0] = '\0';
    list[count].system = true;
    count++;
  }
  snprintf(list[count].name, sizeof(list[count].name), "%s", name);
  snprintf(list[count].secret, sizeof(list[count].secret), "%s", secret);
  list[count].system = false;
  count++;
  if (!SaveAccounts(list, count)) {
    Fail(from, port, 0xc7, "Disc full");
    return;
  }
  fprintf(stderr, "station %u: NEWUSER %s\n", from, name);
  Reply(from, port, body, 2);
}

static void CmdPriv(uint8_t from, uint8_t port, const char* args) {
  struct Account list[MAX_ACCOUNTS];
  bool existed = false;
  int count = LoadAccounts(list, &existed);
  char name[64];
  char flag[16];
  const char* rest = args;
  int at;
  int i;
  int systems = 0;
  bool grant;
  uint8_t body[2] = {0, 0};
  if (!CallerIsSystem(list, count, existed)) {
    Fail(from, port, 0xbd, "Insufficient privilege");
    return;
  }
  if (!NextWord(&rest, name, sizeof(name))) {
    Fail(from, port, 0xfe, "Bad command");
    return;
  }
  Upper(name);
  grant = NextWord(&rest, flag, sizeof(flag)) && (flag[0] == 'S' || flag[0] == 's');
  at = FindAccount(list, count, name);
  if (at < 0) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  if (!grant && list[at].system) {
    for (i = 0; i < count; i++) {
      if (list[i].system) {
        systems++;
      }
    }
    if (systems <= 1) {
      Fail(from, port, 0xbd, "Insufficient privilege");
      return;
    }
  }
  list[at].system = grant;
  if (!SaveAccounts(list, count)) {
    Fail(from, port, 0xc7, "Disc full");
    return;
  }
  Reply(from, port, body, 2);
}

static void CmdFree(uint8_t from, uint8_t port, const char* args);
static void CmdSdisc(uint8_t from, uint8_t port, const char* args);

#define ELF_MACHINE_6502 6502
#define HOST_MAX_ARGS 64
#define HOST_ARG_LEN 1024
#define HOST_OUT_MAX (1024 * 1024)
#define HOST_TEXT_BUDGET 250

#ifdef _WIN32
static bool TryHost(uint8_t from, uint8_t port, int csd, const char* line) {
  (void)from;
  (void)port;
  (void)csd;
  (void)line;
  return false;
}
#else

struct HostOut {
  uint8_t* data;
  int len;
  int cap;
  bool truncated;
  bool cr;
};

static bool HostOutInit(struct HostOut* out) {
  memset(out, 0, sizeof(*out));
  out->cap = 4096;
  out->data = (uint8_t*)malloc((size_t)out->cap);
  if (out->data == NULL) {
    out->cap = 0;
    return false;
  }
  return true;
}

static void HostOutFree(struct HostOut* out) {
  free(out->data);
  out->data = NULL;
  out->cap = 0;
}

static int ElfMachine(const char* path) {
  FILE* fp;
  uint8_t ident[20];
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }
  if (fread(ident, 1, 20, fp) != 20) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F' || ident[5] != 1) {
    return -1;
  }
  return (int)ident[18] | ((int)ident[19] << 8);
}

// A host program is a regular file with an execute bit, reached through a
// symbolic link as well. A 6502 ELF stays on the BBC load-and-run path.
static bool HostExecutable(const char* path) {
  struct stat st;
  int machine;
  uint32_t load;
  uint32_t exec;
  uint32_t length;
  if (path == NULL || path[0] != '/' || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    return false;
  }
  if ((st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
    return false;
  }
  machine = ElfMachine(path);
  if (machine == ELF_MACHINE_6502) {
    return false;
  }
  if (machine >= 0 && ElfImage(path, &load, &exec, &length, NULL)) {
    return false;
  }
  return true;
}

// The current directory wins when it holds a file of this name, executable
// or not. The library is used only when the name is not a file there.
static bool HostFind(int csd, const char* word, char* path, size_t cap) {
  if (Resolve(csd, word, path, cap) && IsFile(path)) {
    return HostExecutable(path);
  }
  if (g_lib != 0 && Resolve(g_lib, word, path, cap) && IsFile(path)) {
    return HostExecutable(path);
  }
  return false;
}

static bool PathMeta(const char* text) {
  for (; *text != '\0'; text++) {
    if (*text == '.' || *text == ',' || *text == '$' || *text == '@' || *text == '&' ||
        *text == '^') {
      return true;
    }
  }
  return false;
}

// ADFS spelling, without the catalogue's ten-character cap and without
// forcing capitals. '$' '@' '&' '^' are the root, the current directory,
// the library, and the parent. A path that already contains '/' is left
// for the caller to keep as typed.
static bool TranslatePath(const char* adfs, char* out, size_t cap, const char* csd) {
  const char* p = adfs;
  size_t used = 0;
  bool any = false;
  out[0] = '\0';
  while (*p != '\0') {
    char decoded[512];
    const char* piece;
    size_t n = 0;
    size_t plen;
    if (*p == '.') {
      p++;
      continue;
    }
    while (*p != '\0' && *p != '.') {
      if (n + 1 >= sizeof(decoded)) {
        return false;
      }
      decoded[n++] = (*p == ',') ? '.' : *p;
      p++;
    }
    decoded[n] = '\0';
    if (*p == '.') {
      p++;
    }
    if (n == 0) {
      continue;
    }
    piece = decoded;
    if (!any && strcmp(decoded, "$") == 0) {
      piece = g_root;
    } else if (!any && strcmp(decoded, "@") == 0) {
      piece = (csd != NULL && csd[0] != '\0') ? csd : g_root;
    } else if (!any && strcmp(decoded, "&") == 0) {
      const char* lib = DirPath(g_lib);
      piece = lib != NULL ? lib : g_root;
    } else if (strcmp(decoded, "^") == 0) {
      piece = "..";
    }
    any = true;
    plen = strlen(piece);
    if (used != 0 && piece[0] != '/') {
      if (used + 1 >= cap) {
        return false;
      }
      out[used++] = '/';
    }
    if (used + plen >= cap) {
      return false;
    }
    memcpy(out + used, piece, plen);
    used += plen;
    out[used] = '\0';
  }
  return used > 0;
}

static void TranslateArg(const char* in, char* out, size_t cap, const char* csd) {
  const char* path = in;
  size_t prefix = 0;
  char translated[HOST_ARG_LEN];
  if (!PathMeta(in)) {
    snprintf(out, cap, "%s", in);
    return;
  }
  if (in[0] == '-') {
    const char* p = in + 1;
    while (*p != '\0' && *p != '.' && *p != ',' && *p != '$' && *p != '@' && *p != '&' &&
           *p != '^') {
      if (*p == '=') {
        p++;
        break;
      }
      p++;
    }
    prefix = (size_t)(p - in);
    path = p;
  }
  if (path[0] == '\0' || strchr(path, '/') != NULL ||
      !TranslatePath(path, translated, sizeof(translated), csd)) {
    snprintf(out, cap, "%s", in);
    return;
  }
  if (prefix >= cap) {
    snprintf(out, cap, "%s", in);
    return;
  }
  memcpy(out, in, prefix);
  snprintf(out + prefix, cap - prefix, "%s", translated);
}

static void HostOutByte(struct HostOut* out, uint8_t b) {
  if (out->len == out->cap) {
    if (out->cap < HOST_OUT_MAX) {
      int ncap = out->cap <= HOST_OUT_MAX / 2 ? out->cap * 2 : HOST_OUT_MAX;
      uint8_t* grown = (uint8_t*)realloc(out->data, (size_t)ncap);
      if (grown != NULL) {
        out->data = grown;
        out->cap = ncap;
      }
    }
    if (out->len == out->cap) {
      int keep = out->cap / 2;
      memmove(out->data, out->data + (out->cap - keep), (size_t)keep);
      out->len = keep;
      out->truncated = true;
    }
  }
  out->data[out->len++] = b;
}

static void HostOutAdd(struct HostOut* out, const uint8_t* buf, int n) {
  int i;
  for (i = 0; i < n; i++) {
    uint8_t b = buf[i];
    if (out->cr) {
      out->cr = false;
      if (b == '\n') {
        continue;
      }
    }
    if (b == '\r') {
      out->cr = true;
      HostOutByte(out, 0x00);
      continue;
    }
    if (b == '\n') {
      HostOutByte(out, 0x00);
      continue;
    }
    if (b == '\t') {
      b = ' ';
    } else if (b < 0x20 || b == 0x7f) {
      continue;
    } else if (b > 0x7e) {
      b = '?';
    }
    HostOutByte(out, b);
  }
}

// Loaded and entered at &0900. The three bytes at HOST_LEN_OFF are the
// text length, and the twelve bytes at HOST_NAME_OFF are its CR-terminated
// name. NFS writes an OSGBPB read straight into the address given to it
// and leaves the count in the control block alone, so this prints the
// number of bytes it asked for and stops when the length above is used up.
// Free space in BASIC is from VARTOP (&02) up to the BASIC stack (&04).
// One read covers the whole text when it fits there. &70-&73 are scratch.
// The fallback buffer is the function-key page at &0B00.
#define HOST_LEN_OFF 402
#define HOST_NAME_OFF 405
#define HOST_NAME_LEN 12
static const uint8_t kHostPrint[] = {
    0xa9, 0x84, 0x20, 0xf4, 0xff, 0xe4, 0x06, 0xd0, 0x52, 0xc4, 0x07, 0xd0,
    0x4e, 0xa9, 0x83, 0x20, 0xf4, 0xff, 0xc4, 0x18, 0xd0, 0x45, 0xa5, 0x13,
    0xc5, 0x18, 0x90, 0x3f, 0xa5, 0x02, 0xc5, 0x12, 0xa5, 0x03, 0xe5, 0x13,
    0x90, 0x35, 0xa5, 0x04, 0xc5, 0x02, 0xa5, 0x05, 0xe5, 0x03, 0x90, 0x2b,
    0xa5, 0x06, 0xc5, 0x04, 0xa5, 0x07, 0xe5, 0x05, 0x90, 0x21, 0x38, 0xa5,
    0x04, 0xe5, 0x02, 0x8d, 0x7e, 0x0a, 0xa5, 0x05, 0xe5, 0x03, 0x8d, 0x7f,
    0x0a, 0x0d, 0x7e, 0x0a, 0xf0, 0x0d, 0xa5, 0x02, 0x8d, 0x7c, 0x0a, 0xa5,
    0x03, 0x8d, 0x7d, 0x0a, 0x4c, 0x6f, 0x09, 0xa9, 0x00, 0x8d, 0x7c, 0x0a,
    0xa9, 0x0b, 0x8d, 0x7d, 0x0a, 0xa9, 0x00, 0x8d, 0x7e, 0x0a, 0xa9, 0x01,
    0x8d, 0x7f, 0x0a, 0xad, 0x92, 0x0a, 0x8d, 0x82, 0x0a, 0xad, 0x93, 0x0a,
    0x8d, 0x83, 0x0a, 0xad, 0x94, 0x0a, 0x8d, 0x84, 0x0a, 0x0d, 0x82, 0x0a,
    0x0d, 0x83, 0x0a, 0xd0, 0x03, 0x4c, 0x72, 0x0a, 0xa9, 0x40, 0xa2, 0x95,
    0xa0, 0x0a, 0x20, 0xce, 0xff, 0xd0, 0x03, 0x4c, 0x72, 0x0a, 0x8d, 0x7b,
    0x0a, 0xad, 0x84, 0x0a, 0xd0, 0x1d, 0xad, 0x7e, 0x0a, 0xcd, 0x82, 0x0a,
    0xad, 0x7f, 0x0a, 0xed, 0x83, 0x0a, 0x90, 0x0f, 0xad, 0x82, 0x0a, 0x8d,
    0x80, 0x0a, 0xad, 0x83, 0x0a, 0x8d, 0x81, 0x0a, 0x4c, 0xcb, 0x09, 0xad,
    0x7e, 0x0a, 0x8d, 0x80, 0x0a, 0xad, 0x7f, 0x0a, 0x8d, 0x81, 0x0a, 0xa0,
    0x0c, 0xa9, 0x00, 0x99, 0x85, 0x0a, 0x88, 0x10, 0xfa, 0xad, 0x7b, 0x0a,
    0x8d, 0x85, 0x0a, 0xad, 0x7c, 0x0a, 0x8d, 0x86, 0x0a, 0xad, 0x7d, 0x0a,
    0x8d, 0x87, 0x0a, 0xad, 0x80, 0x0a, 0x8d, 0x8a, 0x0a, 0xad, 0x81, 0x0a,
    0x8d, 0x8b, 0x0a, 0xa9, 0x04, 0xa2, 0x85, 0xa0, 0x0a, 0x20, 0xd1, 0xff,
    0xad, 0x7c, 0x0a, 0x85, 0x70, 0xad, 0x7d, 0x0a, 0x85, 0x71, 0xad, 0x80,
    0x0a, 0x85, 0x72, 0xad, 0x81, 0x0a, 0x85, 0x73, 0x20, 0x44, 0x0a, 0x38,
    0xad, 0x82, 0x0a, 0xed, 0x80, 0x0a, 0x8d, 0x82, 0x0a, 0xad, 0x83, 0x0a,
    0xed, 0x81, 0x0a, 0x8d, 0x83, 0x0a, 0xad, 0x84, 0x0a, 0xe9, 0x00, 0x8d,
    0x84, 0x0a, 0x0d, 0x82, 0x0a, 0x0d, 0x83, 0x0a, 0xf0, 0x03, 0x4c, 0x9d,
    0x09, 0xa9, 0x00, 0xac, 0x7b, 0x0a, 0x20, 0xce, 0xff, 0x4c, 0x72, 0x0a,
    0xa5, 0x72, 0x05, 0x73, 0xf0, 0x18, 0xa0, 0x00, 0xb1, 0x70, 0x20, 0xe3,
    0xff, 0xe6, 0x70, 0xd0, 0x02, 0xe6, 0x71, 0xa5, 0x72, 0xd0, 0x02, 0xc6,
    0x73, 0xc6, 0x72, 0x4c, 0x44, 0x0a, 0x60, 0xa9, 0x95, 0x8d, 0x85, 0x0a,
    0xa9, 0x0a, 0x8d, 0x86, 0x0a, 0xa2, 0x85, 0xa0, 0x0a, 0x60, 0x20, 0x63,
    0x0a, 0xa9, 0x06, 0x20, 0xdd, 0xff, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x2e, 0x48,
    0x4f, 0x53, 0x54, 0x2e, 0x4f, 0x32, 0x35, 0x34, 0x0d,
};

static bool HostWriteInf(const char* path, uint32_t load, uint32_t exec, uint8_t access) {
  char inf[640];
  FILE* fp;
  InfPath(path, inf, sizeof(inf));
  fp = fopen(inf, "w");
  if (fp == NULL) {
    return false;
  }
  if (fprintf(fp, "%08X %08X %02X\n", load, exec, access) < 0) {
    fclose(fp);
    return false;
  }
  return fclose(fp) == 0;
}

static bool HostShowFile(uint8_t from, uint8_t port, const struct HostOut* out) {
  char dir[640];
  char text_path[640];
  char run_path[640];
  char adfs[32];
  uint8_t image[sizeof(kHostPrint)];
  uint8_t body[48];
  FILE* fp;
  int i;
  int n;
  int wrote;
  int text_len = 0;
  wrote = snprintf(dir, sizeof(dir), "%s/HOST", g_root);
  if (wrote < 0 || wrote >= (int)sizeof(dir)) {
    return false;
  }
  if (MakeDir(dir) != 0 && !IsDir(dir)) {
    return false;
  }
  wrote = snprintf(text_path, sizeof(text_path), "%s/O%u", dir, from);
  if (wrote < 0 || wrote >= (int)sizeof(text_path)) {
    return false;
  }
  wrote = snprintf(run_path, sizeof(run_path), "%s/R%u", dir, from);
  if (wrote < 0 || wrote >= (int)sizeof(run_path)) {
    return false;
  }
  fp = fopen(text_path, "wb");
  if (fp == NULL) {
    return false;
  }
  if (out->truncated) {
    if (fwrite("...\r", 1, 4, fp) != 4) {
      fclose(fp);
      remove(text_path);
      return false;
    }
    text_len += 4;
  }
  for (i = 0; i < out->len; i++) {
    uint8_t b = out->data[i] == 0 ? 0x0d : out->data[i];
    if (fputc(b, fp) == EOF) {
      fclose(fp);
      remove(text_path);
      return false;
    }
    text_len++;
  }
  if (out->len == 0 || out->data[out->len - 1] != 0) {
    if (fputc(0x0d, fp) == EOF) {
      fclose(fp);
      remove(text_path);
      return false;
    }
    text_len++;
  }
  if (fclose(fp) != 0) {
    remove(text_path);
    return false;
  }
  if (chmod(text_path, 0644) != 0) {
    remove(text_path);
    return false;
  }
  n = snprintf(adfs, sizeof(adfs), "$.HOST.O%u", from);
  if (n < 1 || n + 1 > HOST_NAME_LEN) {
    remove(text_path);
    return false;
  }
  memcpy(image, kHostPrint, sizeof(image));
  image[HOST_LEN_OFF] = (uint8_t)text_len;
  image[HOST_LEN_OFF + 1] = (uint8_t)((unsigned)text_len >> 8);
  image[HOST_LEN_OFF + 2] = (uint8_t)((unsigned)text_len >> 16);
  memset(image + HOST_NAME_OFF, 0, HOST_NAME_LEN);
  memcpy(image + HOST_NAME_OFF, adfs, (size_t)n);
  image[HOST_NAME_OFF + n] = 0x0d;
  fp = fopen(run_path, "wb");
  if (fp == NULL) {
    remove(text_path);
    return false;
  }
  if (fwrite(image, 1, sizeof(image), fp) != sizeof(image)) {
    fclose(fp);
    remove(text_path);
    remove(run_path);
    return false;
  }
  if (fclose(fp) != 0) {
    remove(text_path);
    remove(run_path);
    return false;
  }
  if (chmod(run_path, 0644) != 0 || !HostWriteInf(run_path, 0x0900, 0x0900, 0x0d)) {
    char inf[640];
    remove(text_path);
    remove(run_path);
    InfPath(run_path, inf, sizeof(inf));
    remove(inf);
    return false;
  }
  n = snprintf(adfs, sizeof(adfs), "$.HOST.R%u", from);
  if (n < 1 || n + 3 > (int)sizeof(body)) {
    remove(text_path);
    remove(run_path);
    return false;
  }
  body[0] = 8;
  body[1] = 0;
  memcpy(body + 2, adfs, (size_t)n);
  body[2 + n] = 0x0d;
  fprintf(stderr, "station %u: HOST show $.HOST.R%u (%d bytes)\n", from, from, out->len);
  Reply(from, port, body, n + 3);
  return true;
}

static void HostReply(uint8_t from, uint8_t port, struct HostOut* out, int code) {
  uint8_t body[ECONET_MAX_FRAME];
  int budget;
  int start = 0;
  int n;
  int used;
  // NFS and ANFS copy this reply until a carriage return, and the index
  // they use is one byte wide. With no 0x0D in the first 254 bytes the
  // copy never ends and the BBC stays inside the ROM. 0x00 is a new line.
  // 0x80 ends the printing without being shown, so the program's own final
  // new line has to come before it. 0x0D is the copy's stop, after that.
  // Anything longer is handed to the BBC as a file, which is not limited
  // by that copy. If the file cannot be written, the short reply below
  // is still capped so the copy ends.
  if (out->len == 0) {
    if (code == 0) {
      uint8_t quiet[2] = {0, 0};
      Reply(from, port, quiet, 2);
    } else {
      char message[32];
      snprintf(message, sizeof(message), "Exit %d", code);
      Fail(from, port, 0xfe, message);
    }
    return;
  }
  if ((out->len > HOST_TEXT_BUDGET || out->truncated) && HostShowFile(from, port, out)) {
    return;
  }
  if (out->len > HOST_TEXT_BUDGET || out->truncated) {
    fprintf(stderr, "station %u: HOST show failed\n", from);
  }
  budget = HOST_TEXT_BUDGET;
  if (out->truncated || out->len > budget) {
    int room = budget - 4;
    int i;
    if (room < 1) {
      room = 1;
    }
    start = out->len - room;
    if (start < 0) {
      start = 0;
    }
    for (i = start; i < out->len; i++) {
      if (out->data[i] == 0x00) {
        start = i + 1;
        break;
      }
    }
    if (start >= out->len) {
      start = out->len - room;
      if (start < 0) {
        start = 0;
      }
    }
    out->truncated = true;
  }
  body[0] = 4;
  body[1] = 0;
  used = 2;
  if (out->truncated) {
    memcpy(body + used, "...", 3);
    used += 3;
    body[used++] = 0x00;
  }
  n = out->len - start;
  if (n < 0) {
    n = 0;
  }
  if (used + n + 2 > 254) {
    int room = 254 - used - 2;
    if (room < 0) {
      room = 0;
    }
    start += n - room;
    n = room;
  }
  if (n > 0) {
    memcpy(body + used, out->data + start, (size_t)n);
    used += n;
  }
  body[used++] = 0x80;
  body[used++] = 0x0d;
  Reply(from, port, body, used);
}

// Returns 1 when this station has moved on and the program should be
// killed. A repeat of the command, which a slow compile provokes when the
// client sends the line again, is acknowledged and discarded so the
// program keeps running. Its reply uses the latest scout sequence bit.
static int HostNet(uint8_t from, bool* expect_data) {
  uint8_t frame[ECONET_MAX_FRAME];
  int n = RecvRaw(frame, (int)sizeof(frame), 0, false);
  if (n <= 0) {
    return -1;
  }
  if (n < 4 || frame[0] != g_station) {
    return 0;
  }
  if (frame[2] != from) {
    Stash(frame, n);
    return 0;
  }
  if (n == 4) {
    return 0;
  }
  if (!*expect_data && n == 6 && (frame[4] & 0xfe) == 0x80 && frame[5] == FS_COMMAND_PORT) {
    SendAck(frame[2], frame[3]);
    g_scout_ctrl = (uint8_t)(0x80 | (frame[4] & 0x01));
    *expect_data = true;
    return 0;
  }
  if (*expect_data) {
    SendAck(frame[2], frame[3]);
    *expect_data = false;
    return 0;
  }
  Stash(frame, n);
  return 1;
}

static bool TryHost(uint8_t from, uint8_t port, int csd, const char* line) {
  char word[256];
  char path[512];
  char* argv[HOST_MAX_ARGS + 1];
  char* store = NULL;
  const char* cursor;
  const char* cwd;
  struct HostOut out;
  int argc = 0;
  size_t n = 0;
  int pipes[2];
  int status = 0;
  int code = 1;
  pid_t pid;
  bool expect_data = false;
  bool pipe_open = true;
  bool child_done = false;
  bool aborted = false;
  bool have_status = false;
  int i;

  while (*line == ' ') {
    line++;
  }
  if (*line == '\0') {
    return false;
  }
  while (line[n] != '\0' && line[n] != ' ') {
    if (n + 1 >= sizeof(word)) {
      return false;
    }
    word[n] = line[n];
    n++;
  }
  word[n] = '\0';
  if (!HostFind(csd, word, path, sizeof(path))) {
    return false;
  }
  store = (char*)malloc((size_t)HOST_MAX_ARGS * HOST_ARG_LEN);
  if (store == NULL) {
    Fail(from, port, 0xfe, "Bad command");
    return true;
  }
  cwd = DirPath(csd);
  if (cwd == NULL) {
    cwd = g_root;
  }
  argv[argc++] = path;
  cursor = line + n;
  while (*cursor != '\0') {
    char raw[256];
    char* slot;
    const char* start;
    size_t argn = 0;
    while (*cursor == ' ') {
      cursor++;
    }
    if (*cursor == '\0') {
      break;
    }
    if (argc >= HOST_MAX_ARGS) {
      free(store);
      Fail(from, port, 0xfe, "Bad command");
      return true;
    }
    start = cursor;
    while (*cursor != '\0' && *cursor != ' ') {
      cursor++;
      argn++;
    }
    if (argn >= sizeof(raw)) {
      free(store);
      Fail(from, port, 0xfe, "Bad command");
      return true;
    }
    memcpy(raw, start, argn);
    raw[argn] = '\0';
    slot = store + (size_t)argc * HOST_ARG_LEN;
    TranslateArg(raw, slot, HOST_ARG_LEN, cwd);
    argv[argc++] = slot;
  }
  argv[argc] = NULL;
  fprintf(stderr, "station %u: HOST %s", from, path);
  for (i = 1; i < argc; i++) {
    fprintf(stderr, " %s", argv[i]);
  }
  fprintf(stderr, "\n");
  if (pipe(pipes) != 0) {
    free(store);
    Fail(from, port, 0xfe, "Bad command");
    return true;
  }
  pid = fork();
  if (pid < 0) {
    close(pipes[0]);
    close(pipes[1]);
    free(store);
    Fail(from, port, 0xfe, "Bad command");
    return true;
  }
  if (pid == 0) {
    int devnull = open("/dev/null", O_RDONLY);
    setpgid(0, 0);
    if (devnull < 0) {
      _exit(127);
    }
    if (devnull != STDIN_FILENO) {
      if (dup2(devnull, STDIN_FILENO) < 0) {
        _exit(127);
      }
      close(devnull);
    }
    if (dup2(pipes[1], STDOUT_FILENO) < 0 || dup2(pipes[1], STDERR_FILENO) < 0) {
      _exit(127);
    }
    if (pipes[1] != STDOUT_FILENO && pipes[1] != STDERR_FILENO) {
      close(pipes[1]);
    }
    close(pipes[0]);
    if (g_fd > 2) {
      close(g_fd);
    }
    if (chdir(cwd) != 0) {
      _exit(127);
    }
    execv(path, argv);
    _exit(127);
  }
  free(store);
  setpgid(pid, pid);
  close(pipes[1]);
  SocketSetNonBlocking(pipes[0]);
  if (!HostOutInit(&out)) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    close(pipes[0]);
    Fail(from, port, 0xfe, "Bad command");
    return true;
  }
  while (!aborted && !(child_done && !pipe_open)) {
    struct pollfd pfd[2];
    int nfds = 1;
    int rc;
    pfd[0].fd = pipe_open ? pipes[0] : -1;
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    if (g_stash_len == 0) {
      pfd[1].fd = g_fd;
      pfd[1].events = POLLIN;
      pfd[1].revents = 0;
      nfds = 2;
    }
    rc = poll(pfd, (nfds_t)nfds, 200);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (pipe_open && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
      for (;;) {
        uint8_t buf[512];
        ssize_t got = read(pipes[0], buf, sizeof(buf));
        if (got > 0) {
          HostOutAdd(&out, buf, (int)got);
          continue;
        }
        if (got < 0 && errno == EINTR) {
          continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          break;
        }
        pipe_open = false;
        break;
      }
    }
    if (nfds == 2 && (pfd[1].revents & (POLLIN | POLLERR | POLLHUP))) {
      for (;;) {
        int net = HostNet(from, &expect_data);
        if (net < 0) {
          break;
        }
        if (net > 0) {
          aborted = true;
          break;
        }
        if (g_stash_len != 0) {
          break;
        }
      }
    }
    if (!child_done) {
      pid_t got = waitpid(pid, &status, WNOHANG);
      if (got == pid) {
        child_done = true;
        have_status = true;
      } else if (got < 0 && errno != EINTR) {
        child_done = true;
      }
    }
  }
  if (aborted) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    close(pipes[0]);
    HostOutFree(&out);
    fprintf(stderr, "station %u: HOST aborted %s\n", from, path);
    return true;
  }
  if (!child_done) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    close(pipes[0]);
    HostOutFree(&out);
    Fail(from, port, 0xfe, "Bad command");
    return true;
  }
  close(pipes[0]);
  if (have_status && WIFEXITED(status)) {
    code = WEXITSTATUS(status);
  } else if (have_status && WIFSIGNALED(status)) {
    code = 128;
  }
  fprintf(stderr, "station %u: HOST exit %d\n", from, code);
  HostReply(from, port, &out, code);
  HostOutFree(&out);
  return true;
}
#endif

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
  } else if (CommandWord(rest, "LIB") || Two(rest, 'L', 'I')) {
    CmdDir(from, reply, csd, CommandRest(rest), true);
  } else if (CommandWord(rest, "PASS")) {
    CmdPass(from, reply, CommandRest(rest));
  } else if (CommandWord(rest, "NEWUSER") || CommandWord(rest, "NEWU")) {
    CmdNewUser(from, reply, CommandRest(rest));
  } else if (CommandWord(rest, "PRIV")) {
    CmdPriv(from, reply, CommandRest(rest));
  } else if (CommandWord(rest, "FREE")) {
    CmdFree(from, reply, CommandRest(rest));
  } else if (CommandWord(rest, "SDISC") || CommandWord(rest, "SD")) {
    CmdSdisc(from, reply, CommandRest(rest));
  } else if (CommandWord(rest, "RENAME") || CommandWord(rest, "REN")) {
    CmdRename(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "DELETE") || CommandWord(rest, "DESTROY") ||
             Two(rest, 'D', '.')) {
    CmdDelete(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "CDIR") || Two(rest, 'C', 'D')) {
    CmdCdir(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "INFO") || Two(rest, 'I', '.')) {
    CmdInfo(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "EX") || CommandWord(rest, "EXAMINE")) {
    CmdInfo(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "ACCESS") ||
             ((char)toupper((unsigned char)rest[0]) == 'A' &&
              (rest[1] == '.' || rest[1] == '\0' || rest[1] == ' '))) {
    CmdAccess(from, reply, csd, CommandRest(rest));
  } else if (CommandWord(rest, "BYE") || CommandWord(rest, "LOGOFF") ||
             Two(rest, 'B', 'Y')) {
    uint8_t body[2] = {0, 0};
    CloseFiles();
    g_logged = false;
    Reply(from, reply, body, 2);
  } else if (!TryHost(from, reply, csd, rest)) {
    // NFS has no local *RUN. An unknown line is the filename, and command
    // code 8 tells the client to load it (function 5) and jump to the
    // execution address. The name has to start at the third reply byte:
    // the client checks the second byte as the return code, then leaves
    // the rest in place as the function 5 filename. A host program is
    // run here instead, and only a BBC file reaches this reply.
    CmdRun(from, reply, rest);
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
    if (!FileName(name) || !JoinName(path, sizeof(path), dir, ent->d_name)) {
      continue;
    }
    UnixToAdfs(name);
    if (!IsFile(path) && !IsDir(path)) {
      continue;
    }
    at = count;
    while (at > 0 && strcmp(entries[at - 1].name, name) > 0) {
      entries[at] = entries[at - 1];
      at--;
    }
    snprintf(entries[at].name, sizeof(entries[at].name), "%s", name);
    DescribeFile(path, IsDir(path), &entries[at].load, &entries[at].exec, &entries[at].length,
                 &entries[at].access);
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
  char shown[NAME_LEN + 1];
  uint8_t body[40];
  uint8_t reply = payload[0];
  int csd = len > 3 ? payload[3] : g_csd;
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
  ShowLeaf(shown, sizeof(shown), path);
  body[0] = 0;
  body[1] = 0;
  Pad(body + 2, shown, NAME_LEN);
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

static void FinishPut(uint8_t from) {
  uint8_t body[8];
  int slot = g_put_slot;
  g_putting = false;
  if (slot >= 0 && g_file[slot].used && g_file[slot].fp != NULL) {
    uint32_t end = g_put_offset + g_put_valid;
    fflush(g_file[slot].fp);
    if (end > g_file[slot].length) {
      g_file[slot].length = end;
    }
    if (g_put_sequential) {
      g_file[slot].ptr = end;
    }
  }
  body[0] = 0;
  body[1] = 0;
  body[2] = g_put_valid < g_put_count ? 0x80 : 0x00;
  Put24(body + 3, g_put_valid);
  body[6] = 0;
  Reply(from, g_put_reply, body, 7);
}

static void PutData(uint8_t from, const uint8_t* data, int len) {
  uint8_t ack[1] = {0};
  int store;
  FILE* fp;
  if (!g_putting || from != g_put_client || g_put_slot < 0) {
    return;
  }
  if (g_put_got >= g_put_count) {
    FinishPut(from);
    return;
  }
  store = len;
  if ((uint32_t)store > g_put_count - g_put_got) {
    store = (int)(g_put_count - g_put_got);
  }
  fp = g_file[g_put_slot].fp;
  if (store > 0 && fp != NULL &&
      fseek(fp, (long)(g_put_offset + g_put_got), SEEK_SET) == 0) {
    g_put_valid += (uint32_t)fwrite(data, 1, (size_t)store, fp);
  }
  g_put_got += (uint32_t)store;
  // The last block is not acknowledged. NFS has already stopped listening
  // on the data port and is waiting for the completion reply.
  if (g_put_got >= g_put_count) {
    FinishPut(from);
    return;
  }
  Reply(from, g_put_port, ack, 1);
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

static void SendBytes(uint8_t from, uint8_t data_port, const uint8_t* bytes, uint32_t length) {
  uint32_t sent = 0;
  while (sent < length) {
    uint32_t chunk = FS_BLOCK;
    if (chunk > length - sent) {
      chunk = length - sent;
    }
    if (!Transmit(from, data_port, bytes + sent, (int)chunk)) {
      return;
    }
    sent += chunk;
  }
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
  {
    uint8_t* image = NULL;
    uint32_t elf_load = 0;
    uint32_t elf_exec = 0;
    uint32_t elf_len = 0;
    if (ElfImage(path, &elf_load, &elf_exec, &elf_len, &image)) {
      load = elf_load;
      exec = elf_exec;
      length = elf_len;
    } else {
      length = FileLength(path);
      image = NULL;
    }
    Today(date);
    fprintf(stderr, "station %u: LOAD %s%s\n", from, name, image != NULL ? " (ELF)" : "");
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
      if (image != NULL) {
        SendBytes(from, data_port, image, length);
      } else {
        SendFile(from, data_port, path, length);
      }
    }
    free(image);
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
  g_file[i].fp = NULL;
  g_file[i].length = 0;
  if (read_only) {
    uint8_t* image = NULL;
    uint32_t elf_load = 0;
    uint32_t elf_exec = 0;
    uint32_t elf_len = 0;
    FILE* image_file = NULL;
    if (ElfImage(path, &elf_load, &elf_exec, &elf_len, &image)) {
      image_file = tmpfile();
      if (image_file != NULL &&
          fwrite(image, 1, (size_t)elf_len, image_file) == (size_t)elf_len) {
        rewind(image_file);
        g_file[i].fp = image_file;
        g_file[i].length = elf_len;
      } else if (image_file != NULL) {
        fclose(image_file);
      }
      free(image);
    }
    if (g_file[i].fp == NULL) {
      g_file[i].fp = fopen(path, "rb");
    }
  } else if (create) {
    // OPENOUT replaces an existing file. OPENUP (create clear) keeps it.
    g_file[i].fp = fopen(path, "w+b");
  } else {
    g_file[i].fp = fopen(path, "r+b");
  }
  if (g_file[i].fp == NULL) {
    Fail(from, reply, 0xd6, "Not found");
    return;
  }
  g_file[i].used = true;
  g_file[i].write = !read_only;
  g_file[i].ptr = 0;
  if (g_file[i].length == 0) {
    g_file[i].length = FileLength(path);
  }
  snprintf(g_file[i].path, sizeof(g_file[i].path), "%s", path);
  // NFS 3.34 and ANFS 4.25 take the first byte as the handle mask. Zero
  // means the open failed. Bit 0 is the first file; the ROM turns that bit
  // into a channel number. The mask is repeated for a reader that looks
  // further along the reply.
  body[0] = (uint8_t)(1u << i);
  body[1] = 0;
  body[2] = body[0];
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

static void SetObject(uint8_t from, const uint8_t* payload, int len) {
  char name[256];
  char path[512];
  uint8_t body[2] = {0, 0};
  uint8_t reply = payload[0];
  int arg;
  int name_at;
  int csd;
  bool directory;
  uint32_t load = 0;
  uint32_t exec = 0;
  uint8_t access = 0;
  if (!NeedLogin(from, reply) || len < 8) {
    return;
  }
  arg = payload[5];
  csd = payload[3];
  if (arg == 1) {
    name_at = 15;
  } else if (arg == 2 || arg == 3) {
    name_at = 10;
  } else if (arg == 4) {
    name_at = 7;
  } else {
    Fail(from, reply, 0x85, "Invalid function");
    return;
  }
  if (len <= name_at) {
    return;
  }
  CrText(payload + name_at, len - name_at, name, sizeof(name));
  if (!Resolve(csd, name, path, sizeof(path)) || (!IsFile(path) && !IsDir(path))) {
    Fail(from, reply, 0xd6, "Not found");
    return;
  }
  directory = IsDir(path);
  ReadInf(path, directory, &load, &exec, &access);
  if ((arg == 2 || arg == 3) && (access & 0x10) != 0) {
    Fail(from, reply, 0xbd, "Locked");
    return;
  }
  if (arg == 1) {
    load = Get32(payload + 6);
    exec = Get32(payload + 10);
    access = payload[14];
  } else if (arg == 2) {
    load = Get32(payload + 6);
  } else if (arg == 3) {
    exec = Get32(payload + 6);
  } else {
    access = payload[6];
  }
  if (directory) {
    access = (uint8_t)(access | 0x20);
  } else {
    access = (uint8_t)(access & (uint8_t)~0x20);
  }
  WriteInf(path, load, exec, access);
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
    DescribeFile(path, kind == 2, &load, &exec, &length, &access);
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
    char leaf[NAME_LEN + 1];
    if (kind == 0) {
      leaf[0] = '\0';
    } else {
      ShowLeaf(leaf, sizeof(leaf), path);
    }
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
  char csd_leaf[NAME_LEN + 1];
  char lib_leaf[NAME_LEN + 1];
  const char* csd = DirPath(g_csd);
  const char* lib = DirPath(g_lib);
  if (!NeedLogin(from, reply)) {
    return;
  }
  ShowLeaf(csd_leaf, sizeof(csd_leaf), csd);
  ShowLeaf(lib_leaf, sizeof(lib_leaf), lib);
  body[0] = 0;
  body[1] = 0;
  body[2] = (uint8_t)strlen(g_disc);
  Pad(body + 3, g_disc, 16);
  Pad(body + 19, csd_leaf, NAME_LEN);
  Pad(body + 29, lib_leaf, NAME_LEN);
  Reply(from, reply, body, 39);
}

static void DiscBytes(uint64_t* free_bytes, uint64_t* total_bytes) {
  *free_bytes = 0;
  *total_bytes = 0;
#ifndef _WIN32
  {
    struct statvfs st;
    if (statvfs(g_root, &st) == 0) {
      *free_bytes = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
      *total_bytes = (uint64_t)st.f_blocks * (uint64_t)st.f_frsize;
    }
  }
#endif
}

static bool ThisDisc(const char* name) {
  char text[32];
  size_t n = 0;
  while (name[n] != '\0' && name[n] != ' ' && n + 1 < sizeof(text)) {
    text[n] = name[n];
    n++;
  }
  text[n] = '\0';
  if (text[0] == ':') {
    memmove(text, text + 1, strlen(text));
  }
  Upper(text);
  return text[0] == '\0' || strcmp(text, g_disc) == 0;
}

static void CmdFree(uint8_t from, uint8_t port, const char* args) {
  char text[80];
  uint8_t body[96];
  uint64_t free_bytes;
  uint64_t total_bytes;
  size_t n;
  while (*args == ' ') {
    args++;
  }
  if (!ThisDisc(args)) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  DiscBytes(&free_bytes, &total_bytes);
  snprintf(text, sizeof(text), "%s  %lluK free of %lluK", g_disc,
           (unsigned long long)(free_bytes / 1024), (unsigned long long)(total_bytes / 1024));
  n = strlen(text);
  body[0] = 4;
  body[1] = 0;
  memcpy(body + 2, text, n);
  body[2 + n] = 0x80;
  Reply(from, port, body, (int)(3 + n));
}

static void CmdSdisc(uint8_t from, uint8_t port, const char* args) {
  char library[512];
  uint8_t body[6];
  while (*args == ' ') {
    args++;
  }
  if (!ThisDisc(args)) {
    Fail(from, port, 0xd6, "Not found");
    return;
  }
  g_csd = g_urd;
  g_lib = g_urd;
  if (FindChild(g_root, "LIBRARY", library, sizeof(library)) && IsDir(library)) {
    g_lib = DirHandle(library);
  }
  body[0] = 6;
  body[1] = 0;
  body[2] = (uint8_t)g_urd;
  body[3] = (uint8_t)g_csd;
  body[4] = (uint8_t)g_lib;
  fprintf(stderr, "station %u: SDISC %s\n", from, g_disc);
  Reply(from, port, body, 5);
}

static void FreeSpace(uint8_t from, const uint8_t* payload, int len) {
  char name[64];
  uint8_t body[8];
  uint8_t reply = payload[0];
  uint64_t free_bytes;
  uint64_t total_bytes;
  uint32_t free_blocks;
  uint32_t total_blocks;
  if (!NeedLogin(from, reply)) {
    return;
  }
  name[0] = '\0';
  if (len > 5) {
    CrText(payload + 5, len - 5, name, sizeof(name));
  }
  if (!ThisDisc(name)) {
    Fail(from, reply, 0xd6, "Not found");
    return;
  }
  DiscBytes(&free_bytes, &total_bytes);
  free_blocks = (uint32_t)(free_bytes / 256);
  total_blocks = (uint32_t)(total_bytes / 256);
  if (free_blocks > 0xffffffu) {
    free_blocks = 0xffffffu;
  }
  if (total_blocks > 0xffffffu) {
    total_blocks = 0xffffffu;
  }
  // The first byte is 1 when both the free space and the disc size follow.
  body[0] = 1;
  body[1] = 0;
  Put24(body + 2, free_blocks);
  Put24(body + 5, total_blocks);
  Reply(from, reply, body, 8);
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

// Fill a hole when a write starts past the current end.
static bool PadTo(FILE* fp, uint32_t length, uint32_t pos) {
  uint8_t zeros[256];
  if (pos <= length) {
    return true;
  }
  memset(zeros, 0, sizeof(zeros));
  if (fseek(fp, (long)length, SEEK_SET) != 0) {
    return false;
  }
  while (length < pos) {
    uint32_t n = pos - length;
    if (n > sizeof(zeros)) {
      n = (uint32_t)sizeof(zeros);
    }
    if (fwrite(zeros, 1, (size_t)n, fp) != (size_t)n) {
      return false;
    }
    length += n;
  }
  return true;
}

// OSGBPB write. Same header as GetBytes, but the data then arrives from the
// client on the port it named (NFS uses &91). The first reply's first byte
// is the block size NFS will send; the second byte has to stay zero because
// that is the return code.
static void PutBytes(uint8_t from, const uint8_t* payload, int len) {
  uint8_t body[8];
  uint8_t reply;
  int slot;
  uint32_t count;
  uint32_t offset;
  bool sequential;
  if (len < 13) {
    return;
  }
  reply = payload[0];
  if (!NeedLogin(from, reply)) {
    return;
  }
  if (g_putting || g_saving) {
    Fail(from, reply, 0xc2, "Server busy");
    return;
  }
  slot = FileSlot(payload[5]);
  if (slot < 0 || !g_file[slot].write || g_file[slot].fp == NULL) {
    Fail(from, reply, 0x65, "Not open");
    return;
  }
  sequential = payload[6] == 0;
  count = Get24(payload + 7);
  offset = sequential ? g_file[slot].ptr : Get24(payload + 10);
  if (count > 0 && offset > g_file[slot].length) {
    if (!PadTo(g_file[slot].fp, g_file[slot].length, offset)) {
      Fail(from, reply, 0x73, "Beyond file end");
      return;
    }
    g_file[slot].length = offset;
  }
  g_putting = true;
  g_put_client = from;
  g_put_reply = reply;
  g_put_port = payload[2];
  g_put_slot = slot;
  g_put_sequential = sequential;
  g_put_offset = offset;
  g_put_count = count;
  g_put_got = 0;
  g_put_valid = 0;
  body[0] = (uint8_t)FS_PUT_BLOCK;
  body[1] = 0;
  body[2] = g_put_port;
  body[3] = (uint8_t)FS_PUT_BLOCK;
  body[4] = 0;
  Reply(from, reply, body, 5);
  if (count == 0) {
    FinishPut(from);
  }
}

static void Dispatch(uint8_t from, uint8_t scout_port, const uint8_t* payload, int len) {
  uint8_t function;
  if (g_putting && scout_port == g_put_port) {
    PutData(from, payload, len);
    return;
  }
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
  } else if (function == 11) {
    PutBytes(from, payload, len);
  } else if (function == 12) {
    ReadPointer(from, payload, len);
  } else if (function == 13) {
    SetPointer(from, payload, len);
  } else if (function == 17) {
    ReadEof(from, payload, len);
  } else if (function == 18) {
    ObjectInfo(from, payload, len);
  } else if (function == 19) {
    SetObject(from, payload, len);
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
  } else if (function == 26) {
    FreeSpace(from, payload, len);
  } else {
    Fail(from, payload[0], 0x85, "Invalid function");
  }
}

static bool Listening(uint8_t port) {
  if (port == FS_COMMAND_PORT || port == FS_DATA_PORT) {
    return true;
  }
  return g_putting && port == g_put_port;
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
  // A short data block is six bytes on the wire, the same length as a scout.
  // Once a save or a block write has announced itself, the next frame from
  // that station on the data port is the file bytes.
  if (len == 6 && (frame[4] & 0xfe) == 0x80 && Listening(frame[5]) &&
      !(scout_open && frame[2] == scout_from &&
        ((g_saving && scout_port == FS_DATA_PORT && frame[2] == g_save_client) ||
         (g_putting && scout_port == g_put_port && frame[2] == g_put_client)))) {
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
