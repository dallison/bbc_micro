//
//  6502_intepreter.h
//  6502_interpreter
//
//  Created by David Allison on 5/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#ifndef W65C02_interpreter_h
#define W65C02_interpreter_h

#include "loader.h"
#include "6502_machine.h"
#include "6502_debugger.h"

#define W65C02_STACK_SIZE 1024

// Use undefined 65c02 instructions for special purposes.
#define W65C02_BRK 0xef            // syscall handler
#define W65C02_BREAKPOINT 0xff     // Breakpoint.

// Mapped I/O region.
#define W65C02_IO_START 0xfe00
#define W65C02_IO_END 0xfeff

#define W65C02_MAX_OPEN_FILES 10

struct W65C02Interpreter;
struct BbcMachine;

typedef struct W65C02Interpreter {
  Loader* loader;
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
  
  SymbolScope* current_symbol;
  Vector breakpoints;
  Vector watchpoints;
  bool debug;
  bool stop_at_next_instruction;
  Breakpoint* current_bp;
  char last_command[256];
  int next_bp_num;
  int entry_address;
  bool trace;
  Vector devices;
  bool cycle_accurate;
  // Added to the opcode's base cycle count for a taken branch, a page
  // crossing, or a 1 MHz bus access. Cleared at the start of each instruction.
  int extra_cycles;
  int owed_cycles;
  uint64_t cycle_anchor;
  uint64_t cycle_origin;
  uint16_t enter_func;        // Address of __enter (treated specially)
  uint16_t enter_leaf_func;        // Address of __enter_leaf (treated specially)
  uint16_t guest_call_return_pc;
  bool init_arrays_done;
  bool running;
  int exit_code;
  String rom_filename;
  int open_files[W65C02_MAX_OPEN_FILES];
  struct BbcMachine* bbc;
  int bbc_initial_mode;
  const char* bbc_screen_path;
  const char* bbc_os_path;
  const char* bbc_ram_path;
  uint64_t bbc_step_limit;
  uint64_t bbc_steps;
  // Set for a BBC Micro. Cleared by -65c02 so davecc's CMOS opcodes and the
  // $EF syscall stay available on the same machine.
  bool nmos;
  bool bbc_65c02;
  // The Master's G65SC12: the 65SC02 set, with unused opcodes as NOPs.
  // $EF is a one-cycle NOP here, not the davecc syscall.
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
  // Optional interrupt sources for a processor that is not the BBC.
  // A NULL function is absent. write_protect rejects stores at or above
  // that address once no device has claimed the write. Zero leaves every
  // store alone.
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

bool W65C02GuestAddressExecutable(Loader* loader, uint16_t addr);
void W65C02GuestCallVoidFunction(W65C02Interpreter* interpreter, uint16_t fn);
bool W65C02GuestRunInitArrays(Loader* loader, W65C02Interpreter* interpreter);
bool W65C02GuestRunFiniArrays(Loader* loader, W65C02Interpreter* interpreter);

void W65C02InterpreterInit(W65C02Interpreter* interpreter, bool debug,
                           bool cycle_accurate,
                           bool trace, const char* rom_filename);
// Replace the hosted ACIA with BBC Micro FRED/JIM/SHEILA and the video ULA.
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

int W65C02InterpreterRun(W65C02Interpreter* interpreter, Loader* loader,
                         uint64_t entry_address, int argc, char** argv,
                         int first_arg);
void W65C02InterpreterDisassemble(W65C02Interpreter* interpreter, Loader* loader);
void W65C02InterpreterExtract(W65C02Interpreter* interpreter, Loader* loader, FILE* fp);
void W65C02InterpreterDestruct(W65C02Interpreter* interpreter);

void W65C02DisassemblePc(W65C02Interpreter* interpreter);
void W65C02Reset(W65C02Interpreter* interpreter);

#endif /* W65C02_interpreter_h */
