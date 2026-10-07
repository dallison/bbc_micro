//
//  arm_tube.c
//  ARM3 second processor on the Tube.
//
//  Four megabytes of RAM, and the same eight ULA registers the 6502
//  parasite uses, at 0x03000000. The ARM3 runs at 8 MHz, four of its
//  cycles for each 2 MHz cycle of the BBC. Reset copies the client back
//  to address 0, which is where the ARM fetches its first instruction.
//

#include "arm3.h"
#include "bbc_hardware.h"
#include "tube.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARM_RAM (4 * 1024 * 1024)
#define TUBE_BASE 0x03000000u

extern const uint8_t kArmClient[];
extern const int kArmClientLength;

typedef struct {
  Arm3* cpu;
  Tube* tube;
  uint8_t* ram;
  uint8_t* image;
  int image_len;
  int credit;
  bool held;
} ArmParasite;

static uint8_t LoadByte(ArmParasite* parasite, uint32_t addr) {
  addr &= ARM3_ADDR_MASK;
  if (addr >= TUBE_BASE && addr < TUBE_BASE + 8u) {
    return TubeParasiteRead(parasite->tube, (int)(addr - TUBE_BASE));
  }
  if (addr < ARM_RAM) {
    return parasite->ram[addr];
  }
  return 0;
}

static void StoreByte(ArmParasite* parasite, uint32_t addr, uint8_t value) {
  addr &= ARM3_ADDR_MASK;
  if (addr >= TUBE_BASE && addr < TUBE_BASE + 8u) {
    TubeParasiteWrite(parasite->tube, (int)(addr - TUBE_BASE), value);
    return;
  }
  if (addr < ARM_RAM) {
    parasite->ram[addr] = value;
  }
}

static uint32_t LoadWord(void* ctx, uint32_t addr) {
  ArmParasite* parasite = ctx;
  uint32_t value = 0;
  int i;
  for (i = 0; i < 4; i++) {
    value |= (uint32_t)LoadByte(parasite, addr + (uint32_t)i) << (8 * i);
  }
  return value;
}

static void StoreWord(void* ctx, uint32_t addr, uint32_t value) {
  ArmParasite* parasite = ctx;
  int i;
  for (i = 0; i < 4; i++) {
    StoreByte(parasite, addr + (uint32_t)i, (uint8_t)(value >> (8 * i)));
  }
}

static uint8_t LoadByteHook(void* ctx, uint32_t addr) {
  return LoadByte(ctx, addr);
}

static void StoreByteHook(void* ctx, uint32_t addr, uint8_t value) {
  StoreByte(ctx, addr, value);
}

static bool ArmIrq(void* ctx) {
  return TubeParasiteIrq(((ArmParasite*)ctx)->tube);
}

static bool ArmFiq(void* ctx) {
  return TubeParasiteNmi(((ArmParasite*)ctx)->tube);
}

static void ArmFiqAck(void* ctx) {
  TubeParasiteNmiAck(((ArmParasite*)ctx)->tube);
}

static void StartCpu(ArmParasite* parasite) {
  if (parasite->image != NULL && parasite->image_len > 0) {
    memcpy(parasite->ram, parasite->image, (size_t)parasite->image_len);
  }
  Arm3Reset(parasite->cpu);
}

static void ArmReset(void* ctx) {
  ArmParasite* parasite = ctx;
  TubeHardReset(parasite->tube);
  parasite->credit = 0;
  parasite->held = TubeParasiteHeld(parasite->tube);
  if (!parasite->held) {
    StartCpu(parasite);
  }
}

static void ArmRun(void* ctx, int host_cycles) {
  ArmParasite* parasite = ctx;
  int guard = 0;
  if (TubeParasiteHeld(parasite->tube)) {
    parasite->credit = 0;
    parasite->held = true;
    return;
  }
  if (parasite->held) {
    parasite->held = false;
    StartCpu(parasite);
  }
  parasite->credit += host_cycles * 4;
  while (parasite->credit > 0 && guard < 100000) {
    int step = Arm3Step(parasite->cpu);
    if (step <= 0) {
      parasite->credit = 0;
      return;
    }
    parasite->credit -= step;
    guard++;
  }
}

static void ArmDestroy(void* ctx) {
  ArmParasite* parasite = ctx;
  if (parasite == NULL) {
    return;
  }
  free(parasite->ram);
  free(parasite->image);
  Arm3Destroy(parasite->cpu);
  TubeDestroy(parasite->tube);
  free(parasite);
}

static uint8_t ArmRead(void* ctx, uint16_t addr) {
  return ((ArmParasite*)ctx)->ram[addr];
}

static uint16_t ArmPc(void* ctx) {
  return (uint16_t)Arm3Pc(((ArmParasite*)ctx)->cpu);
}

static bool LoadImage(ArmParasite* parasite, const char* path) {
  const uint8_t* source = kArmClient;
  int length = kArmClientLength;
  uint8_t* loaded = NULL;
  FILE* fp;
  long file_length;
  if (path != NULL) {
    fp = fopen(path, "rb");
    if (fp == NULL) {
      fprintf(stderr, "Unable to open Tube ROM %s\n", path);
      return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
      fclose(fp);
      return false;
    }
    file_length = ftell(fp);
    if (file_length < 4 || file_length > 1024 * 1024) {
      fprintf(stderr, "Tube ROM %s must be 4 to 1048576 bytes\n", path);
      fclose(fp);
      return false;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
      fclose(fp);
      return false;
    }
    loaded = malloc((size_t)file_length);
    if (loaded == NULL || fread(loaded, 1, (size_t)file_length, fp) != (size_t)file_length) {
      fprintf(stderr, "Unable to read Tube ROM %s\n", path);
      free(loaded);
      fclose(fp);
      return false;
    }
    fclose(fp);
    source = loaded;
    length = (int)file_length;
  }
  if (length < 4 || length > ARM_RAM) {
    free(loaded);
    return false;
  }
  parasite->image = malloc((size_t)length);
  if (parasite->image == NULL) {
    free(loaded);
    return false;
  }
  memcpy(parasite->image, source, (size_t)length);
  parasite->image_len = length;
  free(loaded);
  return true;
}

bool BbcMachineAttachArmTube(BbcMachine* bbc, const char* rom_path) {
  ArmParasite* parasite;
  BbcTubeHooks hooks;
  if (bbc == NULL) {
    return false;
  }
  parasite = calloc(1, sizeof(*parasite));
  if (parasite == NULL) {
    return false;
  }
  parasite->ram = calloc(1, ARM_RAM);
  parasite->cpu = Arm3Create();
  parasite->tube = TubeCreate();
  if (parasite->ram == NULL || parasite->cpu == NULL || parasite->tube == NULL ||
      !LoadImage(parasite, rom_path)) {
    ArmDestroy(parasite);
    return false;
  }
  Arm3SetMemory(parasite->cpu, LoadWord, StoreWord, LoadByteHook, StoreByteHook, parasite);
  Arm3SetIrq(parasite->cpu, ArmIrq, ArmFiq, ArmFiqAck);
  ArmReset(parasite);
  memset(&hooks, 0, sizeof(hooks));
  hooks.tube = parasite->tube;
  hooks.parasite = parasite;
  hooks.run = ArmRun;
  hooks.reset = ArmReset;
  hooks.destroy = ArmDestroy;
  hooks.read = ArmRead;
  hooks.pc = ArmPc;
  BbcMachineSetTube(bbc, &hooks);
  return true;
}
