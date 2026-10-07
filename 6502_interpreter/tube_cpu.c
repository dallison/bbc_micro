//
//  tube_cpu.c
//  The second processor on the far side of the Tube.
//
//  It has its own 64K. The only shared state is the ULA. A 6502 runs at
//  3 MHz and a 65C02 at 4 MHz, against the BBC's 2 MHz. The client ROM
//  occupies the top of memory and answers the host's register protocol.
//

#include "bbc_hardware.h"
#include "tube.h"
#include "6502_interpreter.h"
#include "6502_devices.h"
#include "vector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t kTubeClient[2048];

typedef struct {
  Device device;
  Tube* tube;
} TubeDev;

typedef struct {
  W65C02Interpreter cpu;
  Tube* tube;
  TubeDev* dev;
  int credit;
  int mul;
  int div;
  bool held;
} Parasite;

static bool TubeDevClaim(Device* dev, uint16_t addr) {
  (void)dev;
  return addr >= 0xFEF8 && addr <= 0xFEFF;
}

static void TubeDevWrite(Device* dev, uint16_t addr, int value) {
  TubeParasiteWrite(((TubeDev*)dev)->tube, addr & 7, (uint8_t)value);
}

static int TubeDevRead(Device* dev, uint16_t addr) {
  return TubeParasiteRead(((TubeDev*)dev)->tube, addr & 7);
}

static bool ParasiteIrqOn(void* ctx) {
  return TubeParasiteIrq(((Parasite*)ctx)->tube);
}

static bool ParasiteNmiOn(void* ctx) {
  return TubeParasiteNmi(((Parasite*)ctx)->tube);
}

static void ParasiteNmiClear(void* ctx) {
  TubeParasiteNmiAck(((Parasite*)ctx)->tube);
}

static void ParasiteResetCpu(Parasite* parasite) {
  parasite->cpu.a = 0;
  parasite->cpu.x = 0;
  parasite->cpu.y = 0;
  parasite->cpu.flags.value = 0;
  parasite->credit = 0;
  W65C02InterpreterResetCpu(&parasite->cpu);
}

static void ParasiteReset(void* ctx) {
  Parasite* parasite = ctx;
  TubeHardReset(parasite->tube);
  parasite->held = TubeParasiteHeld(parasite->tube);
  if (!parasite->held) {
    ParasiteResetCpu(parasite);
  }
}

static void ParasiteDestroy(void* ctx) {
  Parasite* parasite = ctx;
  if (parasite == NULL) {
    return;
  }
  free(parasite->dev);
  VectorDestruct(&parasite->cpu.devices);
  free(parasite->cpu.memory);
  TubeDestroy(parasite->tube);
  free(parasite);
}

static uint8_t ParasiteRead(void* ctx, uint16_t addr) {
  Parasite* parasite = ctx;
  if (parasite->cpu.memory == NULL) {
    return 0;
  }
  return parasite->cpu.memory[addr];
}

static uint16_t ParasitePc(void* ctx) {
  return ((Parasite*)ctx)->cpu.pc;
}

static bool LoadImage(Parasite* parasite, const char* path) {
  const uint8_t* image = kTubeClient;
  size_t length = sizeof(kTubeClient);
  uint8_t* loaded = NULL;
  FILE* fp;
  long file_length;
  uint32_t base;
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
    if (file_length < 6 || file_length > 16384) {
      fprintf(stderr, "Tube ROM %s must be 6 to 16384 bytes\n", path);
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
    image = loaded;
    length = (size_t)file_length;
  }
  base = 0x10000u - (uint32_t)length;
  memcpy(parasite->cpu.memory + base, image, length);
  parasite->cpu.write_protect = (uint16_t)base;
  free(loaded);
  return true;
}

// The host writes a language image without waiting between bytes. A real
// parasite takes each byte in its NMI while the host is still in that
// instruction's delay. Finish that NMI before the host continues, or the
// one-byte register drops the next write.
static void FinishNmi(Parasite* parasite, int depth) {
  int guard = 0;
  while (parasite->cpu.in_irq > depth && guard < 800) {
    if (W65C02InterpreterStepRaw(&parasite->cpu) <= 0) {
      return;
    }
    guard++;
  }
}

static void ParasiteRun(void* ctx, int host_cycles) {
  Parasite* parasite = ctx;
  int guard = 0;
  if (host_cycles < 0) {
    host_cycles = 0;
  }
  if (TubeParasiteHeld(parasite->tube)) {
    parasite->credit = 0;
    parasite->held = true;
    return;
  }
  if (parasite->held) {
    parasite->held = false;
    ParasiteResetCpu(parasite);
  }
  parasite->credit += host_cycles * parasite->mul;
  while (parasite->credit >= parasite->div && guard < 100000) {
    int depth = parasite->cpu.in_irq;
    bool nmi = TubeParasiteNmi(parasite->tube);
    int step = W65C02InterpreterStepRaw(&parasite->cpu);
    if (step <= 0) {
      parasite->credit = 0;
      return;
    }
    parasite->credit -= parasite->div * step;
    guard++;
    if (nmi) {
      FinishNmi(parasite, depth);
    }
  }
}

bool BbcMachineAttachTube(BbcMachine* bbc, int kind, const char* rom_path) {
  Parasite* parasite;
  TubeDev* dev;
  BbcTubeHooks hooks;
  if (bbc == NULL || (kind != BBC_TUBE_6502 && kind != BBC_TUBE_65C02)) {
    return false;
  }
  parasite = calloc(1, sizeof(*parasite));
  if (parasite == NULL) {
    return false;
  }
  parasite->cpu.memory = calloc(65536, 1);
  parasite->tube = TubeCreate();
  dev = calloc(1, sizeof(*dev));
  if (parasite->cpu.memory == NULL || parasite->tube == NULL || dev == NULL) {
    free(parasite->cpu.memory);
    TubeDestroy(parasite->tube);
    free(dev);
    free(parasite);
    return false;
  }
  parasite->dev = dev;
  dev->tube = parasite->tube;
  dev->device.claim = TubeDevClaim;
  dev->device.write = TubeDevWrite;
  dev->device.read = TubeDevRead;
  VectorInit(&parasite->cpu.devices);
  VectorAppend(&parasite->cpu.devices, &dev->device);
  parasite->cpu.zero_page = parasite->cpu.memory;
  parasite->cpu.stack = parasite->cpu.memory + 0x100;
  parasite->cpu.cycle_accurate = false;
  parasite->cpu.allow_turbo = false;
  parasite->cpu.running = true;
  parasite->cpu.irq_pending = ParasiteIrqOn;
  parasite->cpu.nmi_pending = ParasiteNmiOn;
  parasite->cpu.nmi_clear = ParasiteNmiClear;
  parasite->cpu.irq_ctx = parasite;
  if (kind == BBC_TUBE_65C02) {
    parasite->cpu.nmos = false;
    parasite->cpu.sc12 = true;
    parasite->cpu.bbc_65c02 = true;
    parasite->mul = 2;
    parasite->div = 1;
  } else {
    parasite->cpu.nmos = true;
    parasite->mul = 3;
    parasite->div = 2;
  }
  if (!LoadImage(parasite, rom_path)) {
    ParasiteDestroy(parasite);
    return false;
  }
  ParasiteReset(parasite);
  memset(&hooks, 0, sizeof(hooks));
  hooks.tube = parasite->tube;
  hooks.parasite = parasite;
  hooks.run = ParasiteRun;
  hooks.reset = ParasiteReset;
  hooks.destroy = ParasiteDestroy;
  hooks.read = ParasiteRead;
  hooks.pc = ParasitePc;
  BbcMachineSetTube(bbc, &hooks);
  return true;
}
