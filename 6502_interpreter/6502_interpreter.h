//
//  6502_intepreter.h
//  6502_interpreter
//
//  Created by David Allison on 5/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#ifndef W65C02_interpreter_h
#define W65C02_interpreter_h

#include "vector.h"
#include "6502_debugger.h"

// Breakpoint opcode. The BBC machines do not execute this as a CPU
// instruction: the Master treats it as a one-byte NOP unless the debugger
// is switched on.
#define W65C02_BREAKPOINT 0xff

// Mapped I/O region when no BBC machine is attached.
#define W65C02_IO_START 0xfe00
#define W65C02_IO_END 0xfeff

struct W65C02Interpreter;
struct BbcMachine;

typedef struct W65C02Interpreter {
  uint8_t* memory;         // 64K of memory
  uint8_t* zero_page;
  uint8_t* stack;
  uint8_t a;            // Accumulator.
  uint8_t x;            // X index.
  uint8_t y;            // Y index.
  uint8_t s;            // 6502 stack pointer.
  uint16_t pc;          // Program Counter.
  union {
    struct {
      unsigned int c:1;               // Carry flag.
      unsigned int z:1;               // Zero flag.
      unsigned int i:1;               // Interrupt flag.
      unsigned int d:1;               // Decimal flag.
      unsigned int b:1;               // Break flag.
      unsigned int x:1;               // Not used.
      unsigned int v:1;               // Overflow flag.
      unsigned int s:1;               // Sign flag.
    } bits;
    int8_t value;
  } flags;

  Vector breakpoints;
  Vector watchpoints;
  bool debug;
  bool stop_at_next_instruction;
  Breakpoint* current_bp;
  char last_command[256];
  int next_bp_num;
  bool trace;
  Vector devices;
  bool cycle_accurate;
  // Added to the opcode's base cycle count for a taken branch, a page
  // crossing, or a 1 MHz bus access. Cleared at the start of each instruction.
  int extra_cycles;
  int owed_cycles;
  uint64_t cycle_anchor;
  uint64_t cycle_origin;
  bool running;
  struct BbcMachine* bbc;
  int bbc_initial_mode;
  const char* bbc_screen_path;
  const char* bbc_os_path;
  const char* bbc_ram_path;
  uint64_t bbc_step_limit;
  uint64_t bbc_steps;
  // Set for a BBC Micro Model B. Cleared by -65c02, which keeps the CMOS
  // opcodes. The Master's G65SC12 sets sc12 as well.
  bool nmos;
  bool bbc_65c02;
  // The Master's G65SC12: the 65SC02 set, with unused opcodes as NOPs.
  bool sc12;
  bool jammed;
  // Run ahead of the wall clock while the guest is computing. Real time
  // while it waits on the keyboard, the frame, or the speaker.
  // allow_turbo is cleared by -mhz. clock_mhz is the fixed rate; 0 lets a
  // busy guest run ahead of the wall clock.
  bool turbo;
  bool allow_turbo;
  int clock_mhz;
  int in_irq;
  int pace_stores;
  int pace_sheila;
  int pace_watch;
  int pace_cycles;
  int pace_busy;
  int turbo_owed;
  // Interrupt sources for a second processor. A NULL function is absent.
  // write_protect rejects stores at or above that address once no device
  // has claimed the write. Zero leaves every store alone.
  bool (*irq_pending)(void* ctx);
  bool (*nmi_pending)(void* ctx);
  void (*nmi_clear)(void* ctx);
  void* irq_ctx;
  uint16_t write_protect;
} W65C02Interpreter;

// Foreground reads and writes decide whether the guest is working or waiting.
void W65C02NoteStore(W65C02Interpreter* cpu, uint16_t addr);
void W65C02NoteRead(W65C02Interpreter* cpu, uint16_t addr);
// Give the hardware the cycles already executed, before a SHEILA access.
void W65C02CatchUp(W65C02Interpreter* cpu);

void W65C02InterpreterInit(W65C02Interpreter* interpreter, bool debug,
                           bool cycle_accurate, bool trace);
// Replace an empty device list with BBC Micro FRED/JIM/SHEILA and the video ULA.
// initial_mode is 0-7, or -1 to leave the CRTC unprogrammed.
void W65C02InterpreterUseBbc(W65C02Interpreter* interpreter, int initial_mode,
                             const char* screen_ppm, const char* os_path);
bool W65C02InterpreterBbcLoadRom(W65C02Interpreter* interpreter, int slot,
                                 const char* path);
bool W65C02InterpreterBbcLoadDisc(W65C02Interpreter* interpreter, int drive,
                                  const char* path);
bool W65C02InterpreterBbcLoadHardDisc(W65C02Interpreter* interpreter, const char* path);
void W65C02InterpreterBbcSetFdc(W65C02Interpreter* interpreter, int kind);
// Keep the 65C02 when enable is true. The Model B default is the NMOS 6502.
void W65C02InterpreterBbcUse65C02(W65C02Interpreter* interpreter, bool enable);
// Allocate guest RAM, map ROMs, and reset. With no OS image, a small MODE 7
// demo is installed so the display and speaker have something to run.
bool W65C02InterpreterPrepareBbc(W65C02Interpreter* interpreter);
// Hold the guest at a fixed clock. The VIA, video, and sound stay locked
// to the CPU, so a program that counts cycles still sees a BBC Micro,
// played faster. mhz is 1..16.
bool W65C02InterpreterSetClockMhz(W65C02Interpreter* interpreter, int mhz);
// Execute one instruction. Returns the CPU cycles it consumed.
int W65C02InterpreterStep(W65C02Interpreter* interpreter);
// One instruction with no wall-clock wait and no second processor. The
// parasite uses this so the two CPUs cannot call each other.
int W65C02InterpreterStepRaw(W65C02Interpreter* interpreter);
void W65C02InterpreterResetCpu(W65C02Interpreter* interpreter);

void W65C02InterpreterDestruct(W65C02Interpreter* interpreter);

void W65C02DisassemblePc(W65C02Interpreter* interpreter);

#endif /* W65C02_interpreter_h */
