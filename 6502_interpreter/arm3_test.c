//
//  arm3_test.c
//  ARM3 flags live in R15, addresses are 26 bits, and the Tube client
//  announces itself. The language boot is skipped when the Model B ROMs
//  are not on this machine.
//

#include "6502_interpreter.h"
#include "arm3.h"
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

typedef struct {
  uint8_t mem[256];
  uint32_t last_store;
  int stores;
} Mem;

static uint32_t Load32(void* ctx, uint32_t addr) {
  Mem* mem = ctx;
  if (addr + 3 >= sizeof(mem->mem)) {
    return 0;
  }
  return (uint32_t)mem->mem[addr] | ((uint32_t)mem->mem[addr + 1] << 8) |
         ((uint32_t)mem->mem[addr + 2] << 16) | ((uint32_t)mem->mem[addr + 3] << 24);
}

static void Store32(void* ctx, uint32_t addr, uint32_t value) {
  Mem* mem = ctx;
  mem->last_store = addr;
  mem->stores++;
  if (addr + 3 >= sizeof(mem->mem)) {
    return;
  }
  mem->mem[addr] = (uint8_t)value;
  mem->mem[addr + 1] = (uint8_t)(value >> 8);
  mem->mem[addr + 2] = (uint8_t)(value >> 16);
  mem->mem[addr + 3] = (uint8_t)(value >> 24);
}

static uint8_t Load8(void* ctx, uint32_t addr) {
  Mem* mem = ctx;
  if (addr >= sizeof(mem->mem)) {
    return 0;
  }
  return mem->mem[addr];
}

static void Store8(void* ctx, uint32_t addr, uint8_t value) {
  Mem* mem = ctx;
  mem->last_store = addr;
  mem->stores++;
  if (addr < sizeof(mem->mem)) {
    mem->mem[addr] = value;
  }
}

static void Put(Mem* mem, uint32_t addr, uint32_t word) {
  mem->mem[addr] = (uint8_t)word;
  mem->mem[addr + 1] = (uint8_t)(word >> 8);
  mem->mem[addr + 2] = (uint8_t)(word >> 16);
  mem->mem[addr + 3] = (uint8_t)(word >> 24);
}

static void TestFlags(void) {
  Arm3* cpu = Arm3Create();
  Mem mem;
  memset(&mem, 0, sizeof(mem));
  Arm3SetMemory(cpu, Load32, Store32, Load8, Store8, &mem);
  // MOVS r0, #0 sets Z. The flags are in R15, not a separate register.
  Put(&mem, 0, 0xE3B00000u);
  Arm3Step(cpu);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_Z) != 0);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_N) == 0);
  EXPECT(Arm3Reg(cpu, 0) == 0);
  // ADDS r0, r0, #1 clears Z.
  Put(&mem, 4, 0xE2900001u);
  Arm3Step(cpu);
  EXPECT(Arm3Reg(cpu, 0) == 1);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_Z) == 0);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_C) == 0);
  // ADDS r1, r0, #0xFFFFFFFF wraps and sets C and Z. r0 is 1, so add the
  // rotated immediate for -1 by setting r0 to all ones first.
  Arm3SetReg(cpu, 0, 0xFFFFFFFFu);
  Put(&mem, 8, 0xE2901001u);
  Arm3SetReg(cpu, 15, (Arm3Reg(cpu, 15) & ~ARM3_PC_MASK) | 8u);
  Arm3Step(cpu);
  EXPECT(Arm3Reg(cpu, 1) == 0);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_Z) != 0);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_C) != 0);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_V) == 0);
  // MOVS r2, #0x80000000. The rotate writes C from bit 31.
  Put(&mem, 12, 0xE3B02102u);
  Arm3SetReg(cpu, 15, (Arm3Reg(cpu, 15) & ~ARM3_PC_MASK) | 12u);
  Arm3Step(cpu);
  EXPECT(Arm3Reg(cpu, 2) == 0x80000000u);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_N) != 0);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_C) != 0);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_Z) == 0);
  // Reading R15 gives the instruction address plus 8.
  Put(&mem, 16, 0xE1A0000Fu);
  Arm3SetReg(cpu, 15, (Arm3Reg(cpu, 15) & ~ARM3_PC_MASK) | 16u);
  Arm3Step(cpu);
  EXPECT((Arm3Reg(cpu, 0) & ARM3_PC_MASK) == 24u);
  EXPECT((Arm3Reg(cpu, 0) & ~ARM3_PC_MASK) == (Arm3Reg(cpu, 15) & ~ARM3_PC_MASK));
  Arm3Destroy(cpu);
}

static void TestBranchAndSwi(void) {
  Arm3* cpu = Arm3Create();
  Mem mem;
  uint32_t psr;
  memset(&mem, 0, sizeof(mem));
  Arm3SetMemory(cpu, Load32, Store32, Load8, Store8, &mem);
  // B to address 8, skipping the MOV.
  Put(&mem, 0, 0xEA000000u);
  Put(&mem, 4, 0xE3A00001u);
  Put(&mem, 8, 0xE3A00002u);
  Arm3Step(cpu);
  EXPECT(Arm3Pc(cpu) == 8);
  Arm3Step(cpu);
  EXPECT(Arm3Reg(cpu, 0) == 2);
  // BL from 12 to 20. R14 keeps the return address and the current PSR.
  Put(&mem, 12, 0xEB000000u);
  Put(&mem, 20, 0xE3A01007u);
  psr = Arm3Reg(cpu, 15) & ~ARM3_PC_MASK;
  Arm3SetReg(cpu, 15, psr | 12u);
  Arm3Step(cpu);
  EXPECT(Arm3Pc(cpu) == 20);
  EXPECT((Arm3Reg(cpu, 14) & ARM3_PC_MASK) == 16u);
  EXPECT((Arm3Reg(cpu, 14) & 0xFC000003u) == (psr & 0xFC000003u));
  // SWI from user mode enters supervisor at 8 and remembers the old mode.
  Arm3SetReg(cpu, 15, ARM3_F | 0x30u);
  Put(&mem, 0x30, 0xEF000000u);
  Arm3Step(cpu);
  EXPECT(Arm3Pc(cpu) == 8);
  EXPECT((Arm3Reg(cpu, 15) & 3u) == ARM3_MODE_SVC);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_I) != 0);
  EXPECT((Arm3Reg(cpu, 14) & ARM3_PC_MASK) == 0x34u);
  EXPECT((Arm3Reg(cpu, 14) & 3u) == ARM3_MODE_USR);
  Arm3Destroy(cpu);
}

static void TestModeAndBus(void) {
  Arm3* cpu = Arm3Create();
  Mem mem;
  memset(&mem, 0, sizeof(mem));
  Arm3SetMemory(cpu, Load32, Store32, Load8, Store8, &mem);
  // User mode cannot change mode or the interrupt masks with MOVS PC.
  Arm3SetReg(cpu, 15, ARM3_MODE_USR);
  Arm3SetReg(cpu, 0, ARM3_N | ARM3_I | ARM3_F | ARM3_MODE_SVC | 0x100u);
  Put(&mem, 0, 0xE1B0F000u);
  Arm3Step(cpu);
  EXPECT(Arm3Pc(cpu) == 0x100);
  EXPECT((Arm3Reg(cpu, 15) & 3u) == ARM3_MODE_USR);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_I) == 0);
  EXPECT((Arm3Reg(cpu, 15) & ARM3_N) != 0);
  // Supervisor can change mode.
  Arm3SetReg(cpu, 15, ARM3_MODE_SVC | ARM3_I | ARM3_F);
  Arm3SetReg(cpu, 0, ARM3_MODE_IRQ | 0x200u);
  Arm3SetReg(cpu, 15, (Arm3Reg(cpu, 15) & ~ARM3_PC_MASK));
  Arm3Step(cpu);
  EXPECT(Arm3Pc(cpu) == 0x200);
  EXPECT((Arm3Reg(cpu, 15) & 3u) == ARM3_MODE_IRQ);
  // R13 is banked. The supervisor stack comes back with the mode.
  Arm3SetReg(cpu, 15, ARM3_MODE_SVC | ARM3_I | ARM3_F);
  Arm3SetReg(cpu, 13, 0x20000u);
  Arm3SetReg(cpu, 15, ARM3_MODE_USR);
  Arm3SetReg(cpu, 13, 0x1000u);
  Arm3SetReg(cpu, 15, ARM3_MODE_SVC | ARM3_I | ARM3_F);
  EXPECT(Arm3Reg(cpu, 13) == 0x20000u);
  Arm3SetReg(cpu, 15, ARM3_MODE_USR);
  EXPECT(Arm3Reg(cpu, 13) == 0x1000u);
  // A store above 64MB lands in the low 26 bits.
  Arm3SetReg(cpu, 15, ARM3_MODE_SVC | ARM3_I | ARM3_F);
  Arm3SetReg(cpu, 0, 0x04000020u);
  Arm3SetReg(cpu, 1, 0x11223344u);
  Put(&mem, 0, 0xE5801000u);
  Arm3Step(cpu);
  EXPECT(mem.last_store == 0x20);
  EXPECT(Load32(&mem, 0x20) == 0x11223344u);
  // SWP exchanges a register with a word.
  Arm3SetReg(cpu, 0, 0x20u);
  Arm3SetReg(cpu, 1, 0xAABBCCDDu);
  Arm3SetReg(cpu, 2, 0);
  Put(&mem, 4, 0xE1002091u);
  Arm3SetReg(cpu, 15, (Arm3Reg(cpu, 15) & ~ARM3_PC_MASK) | 4u);
  Arm3Step(cpu);
  EXPECT(Arm3Reg(cpu, 2) == 0x11223344u);
  EXPECT(Load32(&mem, 0x20) == 0xAABBCCDDu);
  // STMIA / LDMIA.
  Arm3SetReg(cpu, 2, 0x40u);
  Arm3SetReg(cpu, 1, 0x12345678u);
  Put(&mem, 8, 0xE8820002u);
  Put(&mem, 12, 0xE8920008u);
  Arm3SetReg(cpu, 15, (Arm3Reg(cpu, 15) & ~ARM3_PC_MASK) | 8u);
  Arm3Step(cpu);
  Arm3Step(cpu);
  EXPECT(Arm3Reg(cpu, 3) == 0x12345678u);
  Arm3Destroy(cpu);
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

static void TestBanner(void) {
  W65C02Interpreter cpu;
  uint8_t got[32];
  int n = 0;
  int cycles = 0;
  const char* banner = "\nAcorn ARM3 4MB\n\n\r";
  size_t banner_len = strlen(banner);
  W65C02InterpreterInit(&cpu, false, false, false, NULL);
  W65C02InterpreterUseBbc(&cpu, 7, NULL, NULL);
  EXPECT(cpu.bbc != NULL);
  if (cpu.bbc == NULL || !BbcMachineAttachTube(cpu.bbc, BBC_TUBE_ARM, NULL)) {
    fprintf(stderr, "FAIL ARM tube did not attach\n");
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
      if (a != b) {
        break;
      }
    }
    if (j == n) {
      return 1;
    }
  }
  return 0;
}

static int ParasiteEq(W65C02Interpreter* cpu, const char* path, int addr, int off, int n) {
  FILE* fp = fopen(path, "rb");
  uint8_t buf[16];
  int i;
  if (fp == NULL || n > (int)sizeof(buf)) {
    if (fp != NULL) {
      fclose(fp);
    }
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
    printf("arm tube boot skipped (no ROM directory)\n");
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
    printf("arm tube boot skipped (need OS, BASIC, and DFS)\n");
    BbcRomFileFree(files, count > 0 ? count : 0);
    free(dir);
    return;
  }
  memset(&cpu, 0, sizeof(cpu));
  W65C02InterpreterInit(&cpu, false, false, false, NULL);
  W65C02InterpreterUseBbc(&cpu, -1, NULL, os_path);
  memset(skip_slot, 0, sizeof(skip_slot));
  if (cpu.bbc == NULL || !BbcMachineLoadRomFiles(cpu.bbc, files, count, skip_slot) ||
      !BbcMachineAttachTube(cpu.bbc, BBC_TUBE_ARM, NULL) || !W65C02InterpreterPrepareBbc(&cpu)) {
    fprintf(stderr, "FAIL ARM tube boot did not start\n");
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
    if (!ParasiteEq(&cpu, basic_path, 0x8000, 0, 16) ||
        !ScreenHas(cpu.memory + 0x7c00, 1000, "Acorn ARM3 4MB") ||
        !ScreenHas(cpu.memory + 0x7c00, 1000, "*")) {
      continue;
    }
    break;
  }
  EXPECT(ParasiteEq(&cpu, basic_path, 0x8000, 0, 16));
  EXPECT(ParasiteEq(&cpu, basic_path, 0xbff0, 0x3ff0, 16));
  EXPECT(ScreenHas(cpu.memory + 0x7c00, 1000, "Acorn ARM3 4MB"));
  EXPECT(ScreenHas(cpu.memory + 0x7c00, 1000, "*"));
  if (g_failures != before) {
    fprintf(stderr, "arm tube boot cycles=%d\n", cycles);
    for (i = 0; i < 10; i++) {
      int col;
      fprintf(stderr, "%02d|", i);
      for (col = 0; col < 40; col++) {
        uint8_t ch = cpu.memory[0x7c00 + i * 40 + col] & 0x7f;
        fputc(ch >= 32 && ch < 127 ? ch : ' ', stderr);
      }
      fputc('\n', stderr);
    }
  } else {
    printf("arm tube boot ok (%d host cycles)\n", cycles);
  }
  W65C02InterpreterDestruct(&cpu);
  BbcRomFileFree(files, count);
  free(dir);
}

int main(int argc, char** argv) {
  TestFlags();
  TestBranchAndSwi();
  TestModeAndBus();
  TestBanner();
  TestBoot(argc > 0 ? argv[0] : NULL);
  if (g_failures != 0) {
    fprintf(stderr, "%d arm checks failed\n", g_failures);
    return 1;
  }
  return 0;
}
