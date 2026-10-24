// Headless Revs run: boot from disc, step the CPU, dump the host framebuffer.
#include "6502_interpreter.h"
#include "bbc_hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void StepCycles(W65C02Interpreter* cpu, int cycles) {
  while (cycles > 0) {
    int c = W65C02InterpreterStep(cpu);
    if (c <= 0) {
      break;
    }
    cycles -= c;
  }
}

static bool LetterKey(char ch, int* column, int* row) {
  static const struct {
    char ch;
    int column;
    int row;
  } kLetters[] = {
      {'A', 1, 4}, {'B', 4, 6}, {'C', 2, 5}, {'D', 2, 3}, {'E', 2, 2}, {'F', 3, 4},
      {'G', 3, 5}, {'H', 4, 5}, {'I', 5, 2}, {'J', 5, 4}, {'K', 7, 4}, {'L', 6, 5},
      {'M', 5, 6}, {'N', 5, 5}, {'O', 6, 3}, {'P', 7, 3}, {'Q', 0, 1}, {'R', 3, 3},
      {'S', 1, 5}, {'T', 4, 4}, {'U', 5, 3}, {'V', 3, 6}, {'W', 1, 2}, {'X', 2, 4},
      {'Y', 3, 2}, {'Z', 1, 6},
  };
  size_t i;
  if (ch >= 'a' && ch <= 'z') {
    ch = (char)(ch - 'a' + 'A');
  }
  for (i = 0; i < sizeof(kLetters) / sizeof(kLetters[0]); i++) {
    if (kLetters[i].ch == ch) {
      *column = kLetters[i].column;
      *row = kLetters[i].row;
      return true;
    }
  }
  return false;
}

static bool SymbolKey(char ch, int* column, int* row, bool* shift) {
  struct {
    char ch;
    int column;
    int row;
    bool shift;
  } keys[] = {
      {'*', 8, 4, true},  {'"', 1, 3, true},  {' ', 2, 6, false},
  };
  size_t i;
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (keys[i].ch == ch) {
      *column = keys[i].column;
      *row = keys[i].row;
      *shift = keys[i].shift;
      return true;
    }
  }
  return false;
}

static void TapKey(BbcMachine* bbc, W65C02Interpreter* cpu, int column, int row, bool shift) {
  if (shift) {
    BbcMachineSetKey(bbc, 0, 0, true);
  }
  BbcMachineSetKey(bbc, column, row, true);
  StepCycles(cpu, 120000);
  BbcMachineSetKey(bbc, column, row, false);
  if (shift) {
    BbcMachineSetKey(bbc, 0, 0, false);
  }
  StepCycles(cpu, 400000);
}

static bool DigitKey(char ch, int* column, int* row) {
  static const int kDigit[10][2] = {
      {7, 2}, {0, 3}, {1, 3}, {1, 1}, {2, 1}, {3, 1}, {4, 3}, {4, 2}, {5, 1}, {6, 2},
  };
  if (ch < '0' || ch > '9') {
    return false;
  }
  *column = kDigit[ch - '0'][0];
  *row = kDigit[ch - '0'][1];
  return true;
}

static void TypeText(BbcMachine* bbc, W65C02Interpreter* cpu, const char* text) {
  const char* p;
  for (p = text; *p != '\0'; p++) {
    int column = 0;
    int row = 0;
    bool shift = false;
    if (DigitKey(*p, &column, &row) || LetterKey(*p, &column, &row)) {
      TapKey(bbc, cpu, column, row, false);
    } else if (SymbolKey(*p, &column, &row, &shift)) {
      TapKey(bbc, cpu, column, row, shift);
    }
  }
}

static void PressReturn(BbcMachine* bbc, W65C02Interpreter* cpu) {
  TapKey(bbc, cpu, 9, 4, false);
}

static int count_lit(const BbcMachine* bbc) {
  const uint8_t* fb = BbcFramebuffer(bbc);
  int w = BbcFrameWidth(bbc);
  int h = BbcFrameHeight(bbc);
  int x;
  int y;
  int lit = 0;
  if (fb == NULL || w <= 0 || h <= 0) {
    return 0;
  }
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      const uint8_t* p = fb + ((size_t)y * BBC_FB_WIDTH + (size_t)x) * 3;
      if (p[0] | p[1] | p[2]) {
        lit++;
      }
    }
  }
  return lit;
}

static void dump(const char* path, W65C02Interpreter* cpu) {
  BbcMachinePresentFrame(cpu->bbc, false);
  if (!BbcMachineWritePpm(cpu->bbc, path)) {
    fprintf(stderr, "write failed: %s\n", path);
    return;
  }
  fprintf(stderr,
          "%s: beam_frames=%d revs=%d bitmap=%d size=%dx%d lit=%d turbo=%d\n", path,
          BbcMachineCompletedFrames(cpu->bbc), BbcMachineBeamUlaOverride(cpu->bbc) ? 1 : 0,
          BbcMachineBitmapDisplay(cpu->bbc) ? 1 : 0, BbcFrameWidth(cpu->bbc),
          BbcFrameHeight(cpu->bbc), count_lit(cpu->bbc), cpu->turbo ? 1 : 0);
}

int main(void) {
  W65C02Interpreter cpu;
  BbcRomFile rom_files[BBC_ROM_IMAGE_MAX];
  bool skip_slot[16];
  const char* rom_dir = "/Users/FZDSZZ/bbc_micro/bbc_b_rom_sockets";
  const char* disc = "/Users/FZDSZZ/bbc_games/Disc015-Revs.ssd";
  const char* os_path;
  int rom_count;
  int steps = 0;
  int guard = 0;
  int next_snap = 5000000;
  int i;

  memset(skip_slot, 0, sizeof(skip_slot));
  rom_count = BbcMachineListRomDirectory(rom_dir, rom_files, BBC_ROM_IMAGE_MAX, BBC_FS_ANY);
  if (rom_count < 1) {
    fprintf(stderr, "no ROMs in %s\n", rom_dir);
    return 1;
  }
  os_path = BbcRomOsPath(rom_files, rom_count);
  if (os_path == NULL) {
    fprintf(stderr, "no OS ROM\n");
    return 1;
  }

  W65C02InterpreterInit(&cpu, false, true, false);
  W65C02InterpreterSetClockMhz(&cpu, 2);
  W65C02InterpreterUseBbc(&cpu, -1, NULL, os_path);
  if (cpu.bbc == NULL || !BbcMachineSetModel(cpu.bbc, BBC_MACHINE_B)) {
    return 1;
  }
  if (!BbcMachineLoadRomFiles(cpu.bbc, rom_files, rom_count, skip_slot)) {
    return 1;
  }
  if (!W65C02InterpreterBbcLoadDisc(&cpu, 0, disc)) {
    fprintf(stderr, "disc load failed\n");
    return 1;
  }
  if (!W65C02InterpreterPrepareBbc(&cpu)) {
    return 1;
  }

  StepCycles(&cpu, 8000000);
  TypeText(cpu.bbc, &cpu, "CHAIN \"CAR\"");
  PressReturn(cpu.bbc, &cpu);
  fprintf(stderr, "typed CHAIN CAR (BASIC), stepping...\n");
  StepCycles(&cpu, 20000000);
  TapKey(cpu.bbc, &cpu, 2, 6, false);
  StepCycles(&cpu, 5000000);
  TapKey(cpu.bbc, &cpu, 0, 3, false);
  PressReturn(cpu.bbc, &cpu);
  StepCycles(&cpu, 5000000);
  TypeText(cpu.bbc, &cpu, "40");
  PressReturn(cpu.bbc, &cpu);
  StepCycles(&cpu, 3000000);
  TypeText(cpu.bbc, &cpu, "32");
  PressReturn(cpu.bbc, &cpu);
  StepCycles(&cpu, 3000000);
  PressReturn(cpu.bbc, &cpu);
  StepCycles(&cpu, 3000000);
  TapKey(cpu.bbc, &cpu, 1, 5, false);
  TapKey(cpu.bbc, &cpu, 4, 4, false);
  TapKey(cpu.bbc, &cpu, 1, 6, false);
  for (i = 0; i < 12; i++) {
    StepCycles(&cpu, 6000000);
    TapKey(cpu.bbc, &cpu, 2, 6, false);
  }
  for (i = 0; i < 6; i++) {
    PressReturn(cpu.bbc, &cpu);
    StepCycles(&cpu, 4000000);
  }

  while (steps < 250000000 && guard < 350000000) {
    int c = W65C02InterpreterStep(&cpu);
    if (c <= 0) {
      break;
    }
    steps += c;
    guard++;
    if (BbcMachineBeamUlaOverride(cpu.bbc) && steps % 2000000 < c) {
      fprintf(stderr, "beam ula at steps=%d beam=%d\n", steps,
              BbcMachineCompletedFrames(cpu.bbc));
    }
    if (steps == 60000000 || steps == 90000000 || steps == 120000000 ||
        steps == 180000000 || steps == 240000000) {
      int n;
      for (n = 0; n < 3; n++) {
        PressReturn(cpu.bbc, &cpu);
      }
    }
    if (steps == 150000000) {
      TapKey(cpu.bbc, &cpu, 1, 5, false);
      TapKey(cpu.bbc, &cpu, 4, 4, false);
      TapKey(cpu.bbc, &cpu, 1, 6, false);
    }
    if (steps >= next_snap) {
      char path[64];
      snprintf(path, sizeof(path), "/tmp/revs_%d.ppm", steps);
      dump(path, &cpu);
      next_snap += 10000000;
    }
  }

  dump("/tmp/revs_final.ppm", &cpu);
  fprintf(stderr, "done steps=%d beam_frames=%d\n", steps, BbcMachineCompletedFrames(cpu.bbc));
  W65C02InterpreterDestruct(&cpu);
  BbcRomFileFree(rom_files, rom_count);
  return 0;
}
