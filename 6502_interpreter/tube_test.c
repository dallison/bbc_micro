//
//  tube_test.c
//  The built-in client answers the host, and a Model B with its own ROMs
//  boots BASIC on the second processor. The boot is skipped when those
//  ROMs are not on this machine.
//

#include "6502_interpreter.h"
#include "bbc_hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                          \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      g_failures++;                                                           \
    }                                                                         \
  } while (0)

static void TestBanner(void) {
  W65C02Interpreter cpu;
  uint8_t got[32];
  int n = 0;
  int cycles = 0;
  const char* banner = "\nAcorn TUBE 6502 64K\n\n\r";
  size_t banner_len = strlen(banner);
  W65C02InterpreterInit(&cpu, false, false, false, NULL);
  W65C02InterpreterUseBbc(&cpu, 7, NULL, NULL);
  EXPECT(cpu.bbc != NULL);
  if (cpu.bbc == NULL || !BbcMachineAttachTube(cpu.bbc, BBC_TUBE_6502, NULL)) {
    fprintf(stderr, "FAIL tube client did not attach\n");
    g_failures++;
    W65C02InterpreterDestruct(&cpu);
    return;
  }
  EXPECT(W65C02InterpreterPrepareBbc(&cpu));
  while (cycles < 200000) {
    int step = W65C02InterpreterStep(&cpu);
    if (step <= 0) {
      break;
    }
    cycles += step;
  }
  while (n < (int)sizeof(got) && (BbcMachineRead(cpu.bbc, 0xfee0) & 0x80) != 0) {
    got[n++] = BbcMachineRead(cpu.bbc, 0xfee1);
  }
  EXPECT(n == (int)banner_len + 1);
  if (n >= (int)banner_len) {
    EXPECT(memcmp(got, banner, banner_len) == 0);
  }
  if (n > (int)banner_len) {
    EXPECT(got[banner_len] == 0);
  }
  W65C02InterpreterDestruct(&cpu);
}

static int NameHas(const char* path, const char* needle) {
  const char* base = path;
  const char* slash;
  size_t n;
  size_t i;
  if (path == NULL || needle == NULL) {
    return 0;
  }
  slash = strrchr(path, '/');
  if (slash != NULL && slash[1] != '\0') {
    base = slash + 1;
  }
  n = strlen(needle);
  for (i = 0; base[i] != '\0'; i++) {
    size_t j;
    for (j = 0; j < n; j++) {
      char a = base[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
      }
      if (a == '\0' || a != b) {
        break;
      }
    }
    if (j == n) {
      return 1;
    }
  }
  return 0;
}

static void DumpBoot(W65C02Interpreter* cpu) {
  int i;
  fprintf(stderr, "host pc=%04X parasite pc=%04X\n", cpu->pc, BbcMachineParasitePc(cpu->bbc));
  fprintf(stderr,
          "F000=%02X%02X F020=%02X%02X F002=%02X%02X%02X%02X F007=%02X F008=%02X%02X F030=%02X F031=%02X\n",
          BbcMachineParasiteRead(cpu->bbc, 0xf001),
          BbcMachineParasiteRead(cpu->bbc, 0xf000),
          BbcMachineParasiteRead(cpu->bbc, 0xf021),
          BbcMachineParasiteRead(cpu->bbc, 0xf020),
          BbcMachineParasiteRead(cpu->bbc, 0xf005),
          BbcMachineParasiteRead(cpu->bbc, 0xf004),
          BbcMachineParasiteRead(cpu->bbc, 0xf003),
          BbcMachineParasiteRead(cpu->bbc, 0xf002),
          BbcMachineParasiteRead(cpu->bbc, 0xf007),
          BbcMachineParasiteRead(cpu->bbc, 0xf009),
          BbcMachineParasiteRead(cpu->bbc, 0xf008),
          BbcMachineParasiteRead(cpu->bbc, 0xf030),
          BbcMachineParasiteRead(cpu->bbc, 0xf031));
  fprintf(stderr, "parasite nonzero:");
  for (i = 0; i < 256; i++) {
    int j;
    int hit = 0;
    if (i >= 0xf8) {
      continue;
    }
    for (j = 0; j < 256; j++) {
      if (BbcMachineParasiteRead(cpu->bbc, (uint16_t)(i * 256 + j)) != 0) {
        hit = 1;
        break;
      }
    }
    if (hit) {
      fprintf(stderr, " %02X:%02X", i, j);
    }
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "parasite 8000:");
  for (i = 0; i < 16; i++) {
    fprintf(stderr, " %02X", BbcMachineParasiteRead(cpu->bbc, (uint16_t)(0x8000 + i)));
  }
  fprintf(stderr, "\n0100:");
  for (i = 0; i < 16; i++) {
    fprintf(stderr, " %02X", BbcMachineParasiteRead(cpu->bbc, (uint16_t)(0x0100 + i)));
  }
  fprintf(stderr, "\nscreen:");
  for (i = 0; i < 240; i++) {
    uint8_t ch = cpu->memory[0x7c00 + i] & 0x7f;
    fputc(ch >= 32 && ch < 127 ? (char)ch : '.', stderr);
  }
  fputc('\n', stderr);
}

static int ScreenHas(const uint8_t* screen, int n, const char* text) {
  int len = (int)strlen(text);
  int i;
  if (len <= 0 || len > n) {
    return 0;
  }
  for (i = 0; i <= n - len; i++) {
    int j;
    for (j = 0; j < len; j++) {
      if ((screen[i + j] & 0x7f) != (uint8_t)text[j]) {
        break;
      }
    }
    if (j == len) {
      return 1;
    }
  }
  return 0;
}

static int ParasiteEq(W65C02Interpreter* cpu, const char* path, uint16_t addr, long off, int n) {
  FILE* fp;
  uint8_t buf[32];
  int i;
  if (path == NULL || n <= 0 || n > (int)sizeof(buf)) {
    return 0;
  }
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return 0;
  }
  if (fseek(fp, off, SEEK_SET) != 0 || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
    fclose(fp);
    return 0;
  }
  fclose(fp);
  for (i = 0; i < n; i++) {
    if (BbcMachineParasiteRead(cpu->bbc, (uint16_t)(addr + i)) != buf[i]) {
      return 0;
    }
  }
  return 1;
}

static void TestBoot(const char* argv0) {
  W65C02Interpreter cpu;
  BbcRomFile files[BBC_ROM_IMAGE_MAX];
  char* dir;
  const char* os_path;
  int count;
  int i;
  int cycles = 0;
  int saw_dfs = 0;
  int saw_basic = 0;
  const char* basic_path = NULL;
  bool skip_slot[16];
  const int limit = 4000000;
  int before;
  dir = BbcMachineFindRomDirectory(argv0, BBC_B_ROM_DIRECTORY);
  if (dir == NULL) {
    printf("tube boot skipped (no ROM directory)\n");
    return;
  }
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX, BBC_FS_ANY);
  os_path = count > 0 ? BbcRomOsPath(files, count) : NULL;
  for (i = 0; i < count; i++) {
    if (BbcMachineRomFilingKind(files[i].path) == BBC_FS_DFS) {
      saw_dfs = 1;
    }
    if (NameHas(files[i].path, "basic")) {
      saw_basic = 1;
      basic_path = files[i].path;
    }
  }
  if (os_path == NULL || !saw_dfs || !saw_basic) {
    printf("tube boot skipped (need OS, BASIC, and DFS)\n");
    BbcRomFileFree(files, count > 0 ? count : 0);
    free(dir);
    return;
  }
  memset(&cpu, 0, sizeof(cpu));
  W65C02InterpreterInit(&cpu, false, false, false, NULL);
  W65C02InterpreterUseBbc(&cpu, -1, NULL, os_path);
  memset(skip_slot, 0, sizeof(skip_slot));
  if (cpu.bbc == NULL || !BbcMachineLoadRomFiles(cpu.bbc, files, count, skip_slot) ||
      !BbcMachineAttachTube(cpu.bbc, BBC_TUBE_6502, NULL) || !W65C02InterpreterPrepareBbc(&cpu)) {
    fprintf(stderr, "FAIL tube boot did not start\n");
    g_failures++;
    W65C02InterpreterDestruct(&cpu);
    BbcRomFileFree(files, count);
    free(dir);
    return;
  }
  before = g_failures;
  while (cycles < limit) {
    int step = W65C02InterpreterStep(&cpu);
    if (step <= 0) {
      break;
    }
    cycles += step;
    if ((cycles & 0xfff) != 0) {
      continue;
    }
    if (BbcMachineParasiteRead(cpu.bbc, 0xf007) != 1 ||
        BbcMachineParasitePc(cpu.bbc) == 0 ||
        !ParasiteEq(&cpu, basic_path, 0x8000, 0, 16) ||
        !ParasiteEq(&cpu, basic_path, 0xbff0, 0x3ff0, 16)) {
      continue;
    }
    if (memchr(cpu.memory + 0x7c00, '>', 1000) != NULL) {
      break;
    }
  }
  EXPECT(ParasiteEq(&cpu, basic_path, 0x8000, 0, 16));
  EXPECT(ParasiteEq(&cpu, basic_path, 0x8100, 0x100, 16));
  EXPECT(ParasiteEq(&cpu, basic_path, 0xbff0, 0x3ff0, 16));
  EXPECT(BbcMachineParasitePc(cpu.bbc) != 0);
  EXPECT(BbcMachineParasiteRead(cpu.bbc, 0x020e) != 0 ||
         BbcMachineParasiteRead(cpu.bbc, 0x020f) != 0);
  EXPECT(memchr(cpu.memory + 0x7c00, '>', 1000) != NULL);
  EXPECT(ScreenHas(cpu.memory + 0x7c00, 1000, "Acorn TUBE 6502 64K"));
  if (g_failures != before) {
    fprintf(stderr, "tube boot cycles=%d\n", cycles);
    DumpBoot(&cpu);
  } else {
    printf("tube boot ok (%d host cycles)\n", cycles);
  }
  W65C02InterpreterDestruct(&cpu);
  BbcRomFileFree(files, count);
  free(dir);
}

int main(int argc, char** argv) {
  TestBanner();
  TestBoot(argc > 0 ? argv[0] : NULL);
  if (g_failures != 0) {
    fprintf(stderr, "%d tube checks failed\n", g_failures);
    return 1;
  }
  return 0;
}
