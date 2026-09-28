#include "6502_nmos.h"

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

int main(void) {
  W65C02Interpreter cpu;
  uint8_t* mem = calloc(65536, 1);
  int cycles;
  EXPECT(mem != NULL);
  memset(&cpu, 0, sizeof(cpu));
  cpu.memory = mem;

  cpu.a = 1;
  CpuAdd(&cpu, 1);
  EXPECT(cpu.a == 2);
  EXPECT(cpu.flags.bits.z == 0);

  cpu.nmos = false;
  cpu.flags.bits.d = 1;
  cpu.flags.bits.c = 0;
  cpu.a = 0x99;
  CpuAdd(&cpu, 1);
  EXPECT(cpu.a == 0x00);
  EXPECT(cpu.flags.bits.z == 1);
  EXPECT(cpu.flags.bits.s == 0);
  EXPECT(cpu.flags.bits.c == 1);

  cpu.nmos = true;
  cpu.flags.bits.d = 1;
  cpu.flags.bits.c = 0;
  cpu.a = 0x99;
  CpuAdd(&cpu, 1);
  EXPECT(cpu.a == 0x00);
  EXPECT(cpu.flags.bits.z == 0);
  EXPECT(cpu.flags.bits.s == 1);
  EXPECT(cpu.flags.bits.c == 1);

  cpu.flags.bits.c = 0;
  cpu.a = 0x58;
  CpuAdd(&cpu, 0x46);
  EXPECT(cpu.a == 0x04);
  EXPECT(cpu.flags.bits.c == 1);

  cpu.flags.bits.d = 0;
  cpu.a = 0;
  cpu.pc = 0x200;
  mem[0x200] = 0x07;
  mem[0x201] = 0x10;
  mem[0x10] = 0x01;
  cycles = NmosExecute(&cpu, 0x07);
  EXPECT(cycles > 0);
  EXPECT(mem[0x10] == 0x02);
  EXPECT(cpu.a == 0x02);

  cpu.pc = 0x300;
  mem[0x300] = 0xa9;
  EXPECT(NmosExecute(&cpu, 0xa9) < 0);
  EXPECT(cpu.pc == 0x300);

  cpu.pc = 0x400;
  mem[0x400] = 0x02;
  EXPECT(NmosExecute(&cpu, 0x02) > 0);
  EXPECT(cpu.jammed);
  EXPECT(cpu.pc == 0x400);

  cpu.jammed = false;
  cpu.pc = 0x500;
  mem[0x500] = 0x1a;
  EXPECT(NmosExecute(&cpu, 0x1a) > 0);
  EXPECT(cpu.pc == 0x501);

  cpu.pc = 0x600;
  mem[0x600] = 0x80;
  mem[0x601] = 0x10;
  EXPECT(NmosExecute(&cpu, 0x80) > 0);
  EXPECT(cpu.pc == 0x602);

  free(mem);
  if (g_failures != 0) {
    fprintf(stderr, "%d NMOS checks failed\n", g_failures);
    return 1;
  }
  return 0;
}
