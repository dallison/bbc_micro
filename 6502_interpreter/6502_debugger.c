//
//  6502_debugger.c
//  6502_interpreter
//
//  Created by David Allison on 6/4/21.
//  Copyright © 2021 David Allison. All rights reserved.
//

#include "6502_debugger.h"
#include "6502_interpreter.h"
#include <inttypes.h>

enum DebugCommand {
  kDbgNone,
  kDbgEmpty,
  kDbgQuit,
  kDbgRegs,
  kDbgMemory,
  kDbgSetBreak,
  kDbgSetWatch,
  kDbgDelBreak,
  kDbgListBreak,
  kDbgPrint,
  kDbgContinue,
  kDbgRun,
  kDbgStep,
  kDbgStepOver,
  kDbgDasm,
  kDbgTrace,
  kDbgHelp,
  kDbgFinish,
};

static struct {
  const char* spelling;
  enum DebugCommand cmd;
} commands[] = {
  {"quit", kDbgQuit},
  {"q", kDbgQuit},
  {"regs", kDbgRegs},
  {"mem", kDbgMemory},
  {"dump", kDbgMemory},
  {"p", kDbgPrint},
  {"print", kDbgPrint},
  {"b", kDbgSetBreak},
  {"watch", kDbgSetWatch},
  {"run", kDbgRun},
  {"c", kDbgContinue},
  {"s", kDbgStep},
  {"n", kDbgStepOver},
  {"dasm", kDbgDasm},
  {"del", kDbgDelBreak},
  {"bps", kDbgListBreak},
  {"trace", kDbgTrace},
  {"finish", kDbgFinish},
  {"fin", kDbgFinish},
  {"help", kDbgHelp},
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void Help() {
  printf("%-20s Quit interpreter\n", "quit");
  printf("%-20s Show Registers\n", "regs");
  printf("%-20s Hex dump memory\n", "mem [addr] [len");
  printf("%-20s Hex dump memory\n", "dump [addr] [len");
  printf("%-20s Disassemble\n", "dasm [addr] [len]");
  printf("%-20s Set breakpoint at addr\n", "b addr");
  printf("%-20s Set watchpoint at addr\n", "watch addr [:span] [r|w] [value]");
  printf("%-20s Delete break/watchpoint number\n", "del bpnum");
  printf("%-20s List break/watchpoints\n", "bps");
  printf("%-20s tRun from beginning\n", "run");
  printf("%-20s Continue\n", "c");
  printf("%-20s Single step\n", "s");
  printf("%-20s Single step over\n", "n");
  printf("%-20s Switch tracing on or off\n", "trace on|off");
  printf("%-20s Print value of symbol\n", "p sym");
  printf("%-20s Print value of symbol\n", "print sym");
  printf("%-20s Return from current subroutine\n", "finish");
  printf("%-20s Return from current subroutine\n", "fin");
}

static int FindSymbolAddress(W65C02Interpreter* interpreter, const char* name) {
  return (int)LoaderLookupSymbol(interpreter->loader, name);
}

static enum DebugCommand ParseCommand(const char** cmd) {
  const char* p = *cmd;
  char buf[256];
  char* s = buf;
  while (!isblank(*p) && *p != '\n') {
    *s++ = *p++;
  }
  *s = '\0';
  if (buf[0] == '\0') {
    return kDbgEmpty;
  }
  for (int i = 0; i < NUM_COMMANDS; i++) {
    if (strcmp(commands[i].spelling, buf) == 0) {
      *cmd = p;
      return commands[i].cmd;
    }
  }
  return kDbgNone;
}

void DebuggerHexdump(W65C02Interpreter* interpreter, const void* addr, int size) {
  char buf[16];
  const char* caddr = (const char*)addr;
  const char* endaddr = caddr + size;
  int len = size;
  while (len > 0) {
    int iaddr = (int)((char*)caddr - (char*)interpreter->memory);
    printf("%04X  ", iaddr);
    int filled_bytes = 0;
    for (int i = 0; i < 16; i++) {
      if (caddr < endaddr) {
         buf[i] = *caddr++;
        filled_bytes++;
        printf("%02X ", buf[i] & 0xff);
      } else {
        printf("   ");
      }
    }
    printf("  ");
    for (int i = 0; i < filled_bytes; i++) {
      if (buf[i] >= 0x20 && buf[i] < 0x7f) {
        printf("%c", buf[i]);
      } else {
        printf("%c", '.');
      }
    }
    len -= 16;
    printf("\n");
  }
}


static void DumpMemory(W65C02Interpreter* interpreter, const char* tail) {
  int start = 0;
  int len = 20;
  
  while (isspace(*tail)) {
    tail++;
  }
  char buf[256];
  char* p = buf;
  while (!isblank(*tail) && *tail != '\n' && *tail != '\0') {
    *p++ = *tail++;
  }
  *p = '\0';
  bool named_reg = false;
  if (buf[0] == '$') {
    bool is_addr;
    int reglen;
    named_reg = NamedReg(buf, &start, &reglen, &is_addr);
    if (named_reg) {
      if (is_addr) {
        start = *(uint16_t*)&interpreter->memory[start];
        len = 32;
      } else {
        len = reglen;
      }
    }
  }
  if (!named_reg) {
    if (isalpha(buf[0]) || *buf == '_') {
      // Symbol.
      start = FindSymbolAddress(interpreter, buf);
    }
    if (start == 0) {
      start = (int)strtoll(buf, NULL, 16);    // Hex
    }
    while (isspace(*tail)) {
      tail++;
    }
    p = buf;
    while (!isblank(*tail) && *tail != '\n' && *tail != '\0') {
      *p++ = *tail++;
    }
    *p = '\0';
    if (buf[0] != '\0') {
      len = (int)strtoll(buf, NULL, 0);
    }
  }
  DebuggerHexdump(interpreter, &interpreter->memory[start], len);
}

static void PrintFlag(int v, const char nm) {
  if (v) {
    putchar(nm);
  } else {
    putchar(tolower(nm));
  }
}

static void DumpRegs(W65C02Interpreter* interpreter, const char* tail) {
  printf("A:  0x%02x\n", interpreter->a & 0xff);
  printf("X:  0x%02x\n", interpreter->x & 0xff);
  printf("Y:  0x%02x\n", interpreter->y & 0xff);
  printf("S:  0x%02x\n", interpreter->s & 0xff);
  printf("PC: 0x%02x\n", interpreter->pc  & 0xffff);
  printf("P:  ");
  PrintFlag(interpreter->flags.bits.c, 'C');
  PrintFlag(interpreter->flags.bits.z, 'Z');
  PrintFlag(interpreter->flags.bits.i, 'I');
  PrintFlag(interpreter->flags.bits.d, 'D');
  PrintFlag(interpreter->flags.bits.b, 'B');
  PrintFlag(interpreter->flags.bits.x, 'X');
  PrintFlag(interpreter->flags.bits.v, 'V');
  PrintFlag(interpreter->flags.bits.s, 'S');
  printf("\n");
}

Breakpoint* NewBreakpoint(int num, int addr, char old_value, bool temp) {
  Breakpoint* bp = malloc(sizeof(Breakpoint));
  bp->base.num = num;
  bp->base.addr = addr;
  bp->old_value = old_value;
  bp->enabled = false;
  bp->is_temp = temp;
  return bp;
}

Watchpoint* NewWatchpoint(int num, int addr, int span, WatchMode mode, bool any_value, uint8_t value) {
  Watchpoint* wp = malloc(sizeof(Watchpoint));
  wp->base.num = num;
  wp->base.addr = addr;
  wp->span = span;
  wp->mode = mode;
  wp->any_value = any_value;
  wp->value = value;
  return wp;
}

void BreakpointDestruct(Breakpoint* bp) {
}

void ControlPointDelete(ControlPoint* p) {
  free(p);
}

void BreakpointInstall(W65C02Interpreter* interpreter, Breakpoint* bp) {
  interpreter->memory[bp->base.addr] = W65C02_BREAKPOINT;
  bp->enabled = true;
}

void BreakpointUninstall(W65C02Interpreter* interpreter, Breakpoint* bp) {
  interpreter->memory[bp->base.addr] = bp->old_value;
  bp->enabled = false;
}

Breakpoint* FindBreakpoint(W65C02Interpreter* interpreter, int addr) {
  for (size_t i = 0; i < interpreter->breakpoints.length; i++) {
    Breakpoint* bp = interpreter->breakpoints.value.p[i];
    if (bp->base.addr == addr && bp->enabled) {
      return bp;
    }
  }
  return NULL;
}

Watchpoint* FindWatchpoint(W65C02Interpreter* interpreter, int addr) {
  for (size_t i = 0; i < interpreter->watchpoints.length; i++) {
    Watchpoint* wp = interpreter->watchpoints.value.p[i];
    if (wp->base.addr == addr) {
      return wp;
    }
  }
  return NULL;
}

static void CreateBreakpoint(W65C02Interpreter* interpreter, int addr, bool temp) {
  Breakpoint* bp = FindBreakpoint(interpreter, addr);
  if (bp != NULL) {
    printf("Breakpoint %d already set at this address, ignoring\n", bp->base.num);
    return;
  }
  bp = NewBreakpoint(interpreter->next_bp_num++, addr, interpreter->memory[addr], temp);
  VectorAppend(&interpreter->breakpoints, bp);
  BreakpointInstall(interpreter, bp);
  if (!temp) {
    printf("Breakpoint %d set at address 0x%04x\n", bp->base.num, bp->base.addr);
  }
}

static void CreateWatchpoint(W65C02Interpreter* interpreter, int addr, int span, WatchMode mode, bool any_value, int8_t value) {
  Watchpoint* wp = FindWatchpoint(interpreter, addr);
  if (wp != NULL) {
    printf("Watchpoint %d already set at this address, ignoring\n", wp->base.num);
    return;
  }
  wp = NewWatchpoint(interpreter->next_bp_num++, addr, span, mode, any_value, value);
  VectorAppend(&interpreter->watchpoints, wp);
  if (any_value) {
    printf("Watchpoint %d set at address 0x%x +%d for %s\n", wp->base.num, addr, span, mode == kWatchRead ? "read" : "write");
  } else {
    printf("Watchpoint %d set at address 0x%x +%d for %s with value %d\n", wp->base.num, addr, span, mode == kWatchRead ? "read" : "write", value);
  }
}

static void SetBreakpoint(W65C02Interpreter* interpreter, const char* tail) {
  while (isspace(*tail)) {
    tail++;
  }
  if (*tail == '*') {
    // By address.
    tail++;
    uint64_t addr = strtoull(tail, NULL, 16);
    if (addr == 0) {
      printf("Invalid breakpoint address\n");
      return;
    }
    CreateBreakpoint(interpreter, (int)addr, false);
    return;
  }
  char symbol_name[256];
  char* s = symbol_name;
  while (*tail != '\0' && !isspace(*tail)) {
    *s++ = *tail++;
  }
  *s = 0;
  int addr = FindSymbolAddress(interpreter, symbol_name);
  if (addr == 0) {
    printf("Unknown symbol %s\n", symbol_name);
    return;
  }
  CreateBreakpoint(interpreter, (int)addr, false);;
}

static void SetWatchpoint(W65C02Interpreter* interpreter, char* tail) {
  while (isspace(*tail)) {
    tail++;
  }
  int addr;
  if (*tail == '*') {
    // By address.
    tail++;
    addr = (int)strtoul(tail, &tail, 16);
    if (addr == 0) {
      printf("Invalid watchpoint address\n");
      return;
    }
  } else {
    char symbol_name[256];
    char* s = symbol_name;
    while (*tail != '\0' && !isspace(*tail)) {
      *s++ = *tail++;
    }
    *s = 0;
    addr = FindSymbolAddress(interpreter, symbol_name);
    if (addr == 0) {
      printf("Unknown symbol %s\n", symbol_name);
      return;
    }
  }
  while (isspace(*tail)) {
    tail++;
  }
  // Allow +span
  int span = 0;
  bool any_value = true;
  int8_t value = 0;
  WatchMode mode = kWatchWrite;
  if (*tail == '+') {
    char spanbuf[16];
    tail++;
    char* s = spanbuf;
    while (*tail != '\0' && isdigit(*tail)) {
      *s++ = *tail++;
    }
    span = (int)strtoul(spanbuf, NULL, 0);
  }
 
  while (isspace(*tail)) {
    tail++;
  }
  // Check for 'w' or 'r':
  if (*tail == 'r') {
    mode = kWatchRead;
    tail++;
  } else  if (*tail == 'w') {
    mode = kWatchWrite;
    tail++;
  }
  // If we have another arg we have a value
  while (isspace(*tail)) {
    tail++;
  }
  if (*tail != '\0') {
    any_value = false;
    value = (int)strtol(tail, NULL, 0);
  }
  CreateWatchpoint(interpreter, addr, span, mode, any_value, value);
}

void RemoveBreakpoint(W65C02Interpreter* interpreter, Breakpoint* bp) {
  for (size_t i = 0; i < interpreter->breakpoints.length; i++) {
    Breakpoint* b = interpreter->breakpoints.value.p[i];
    if (b ==  bp) {
      VectorDeleteElement(&interpreter->breakpoints, i);
      return;
    }
  }
  printf("No such breakpoint %p\n", bp);
}

static void RemoveBreakpointByNumber(W65C02Interpreter* interpreter, int num) {
  for (size_t i = 0; i < interpreter->breakpoints.length; i++) {
    Breakpoint* bp = interpreter->breakpoints.value.p[i];
    if (bp->base.num ==  num) {
      if (bp->enabled) {
        BreakpointUninstall(interpreter, bp);
      }
      VectorDeleteElement(&interpreter->breakpoints, i);
      return;
    }
  }
  for (size_t i = 0; i < interpreter->watchpoints.length; i++) {
    Watchpoint* wp = interpreter->watchpoints.value.p[i];
    if (wp->base.num ==  num) {
      VectorDeleteElement(&interpreter->watchpoints, i);
      return;
    }
  }
  printf("No such breakpoint/watchpoint %d\n", num);
}


static void DeleteBreakpoint(W65C02Interpreter* interpreter, const char* tail) {
  while (isspace(*tail)) {
    tail++;
  }
  if (*tail == '\0') {
    printf("Delete all breakpoints (y/n)?");
    fflush(stdout);
    char ch = fgetc(stdin);
    if (ch == 'y' || ch == 'Y') {
      for (size_t i = 0; i < interpreter->breakpoints.length; i++) {
        Breakpoint* bp = interpreter->breakpoints.value.p[i];
        if (bp->enabled) {
          BreakpointUninstall(interpreter, bp);
        }
        ControlPointDelete(&bp->base);
      }
      VectorClear(&interpreter->breakpoints);
      for (size_t i = 0; i < interpreter->watchpoints.length; i++) {
        Watchpoint* wp = interpreter->watchpoints.value.p[i];
        ControlPointDelete(&wp->base);
      }
      VectorClear(&interpreter->watchpoints);
      return;
    }
  }
  int bpnum = (int)strtoll(tail, NULL, 0);
  RemoveBreakpointByNumber(interpreter, bpnum);
}

static void ListBreakpoints(W65C02Interpreter* interpreter, const char* tail) {
  for (size_t i = 0; i < interpreter->breakpoints.length; i++) {
    Breakpoint* p = interpreter->breakpoints.value.p[i];
    printf("%-2d breakpoint 0x%x\n", p->base.num, p->base.addr);
  }
  for (size_t i = 0; i < interpreter->watchpoints.length; i++) {
    Watchpoint* p = interpreter->watchpoints.value.p[i];
    if (p->any_value) {
      printf("%-2d watchpoint 0x%x +%d %s\n", p->base.num, p->base.addr, p->span, p->mode == kWatchRead ? "read" : "write");
    } else {
      printf("%-2d watchpoint 0x%x +%d %s %d\n", p->base.num, p->base.addr, p->span, p->mode == kWatchRead ? "read" : "write", p->value);
    }
  }
}

static void Step(W65C02Interpreter* interpreter, const char* tail) {
  interpreter->stop_at_next_instruction = true;
}

static void StepOver(W65C02Interpreter* interpreter, const char* tail) {
  if (interpreter->memory[interpreter->pc] == 0x20) {
    // JSR instruction.
    uint16_t subroutine = interpreter->memory[interpreter->pc+1] |
    interpreter->memory[interpreter->pc+2] << 8;
    uint16_t next;
    if (subroutine == interpreter->enter_func ||
        subroutine == interpreter->enter_leaf_func) {
      next = interpreter->pc + 6;       // Space for the reg save mask.
    } else {
      next = interpreter->pc + 3;
    }
    // Create temp breakpoint at next address (after JSR).
    CreateBreakpoint(interpreter, next, true);
    interpreter->stop_at_next_instruction = false;
    return;
  }
  interpreter->stop_at_next_instruction = true;
}

static void Finish(W65C02Interpreter* interpreter) {
  uint16_t ret_addr = interpreter->memory[0x100 + interpreter->s+1] | (interpreter->memory[0x100+interpreter->s+2] << 8);
  ret_addr++;     // Address on stack is one less than next instruction.
  printf("Running until current subroutine returns to address 0x%x\n", ret_addr & 0xffff);
  CreateBreakpoint(interpreter, ret_addr, true);
  interpreter->stop_at_next_instruction = false;
}

static void Continue(W65C02Interpreter* interpreter, const char* tail) {
  interpreter->stop_at_next_instruction = false;
}

static void Print(W65C02Interpreter* interpreter, const char* tail) {
  char symbol_name[256];
  char* s = symbol_name;
  while (*tail != '\0' && isspace(*tail)) {
    tail++;
  }
  while (*tail != '\0' && !isspace(*tail)) {
    *s++ = *tail++;
  }
  *s = 0;
  if (symbol_name[0] == '$') {
    int addr, len;
    bool is_addr;
    if (NamedReg(symbol_name, &addr, &len, &is_addr)) {
      switch (len) {
        case 1: {
          uint8_t value = *(uint8_t*)&interpreter->memory[addr];
          printf("%s: 0x%x (%d)\n", symbol_name, value, value);
          break;
        }
        case 2: {
          uint16_t value = *(uint16_t*)&interpreter->memory[addr];
          printf("%s: 0x%x (%d)\n", symbol_name, value, value);
          break;
        }
        case 4: {
          uint32_t value = *(uint32_t*)&interpreter->memory[addr];
          printf("%s: 0x%x (%d)\n", symbol_name, value, value);
          break;
        }
        case 8: {
          uint64_t value = *(uint64_t*)&interpreter->memory[addr];
          printf("%s: 0x%" PRIx64 " (%" PRId64 ")\n", symbol_name, value, value);
          break;
        }
        default:
          printf("Bad reg length %d\n", len);
      }
      return;
    }
  }
  int addr = FindSymbolAddress(interpreter, symbol_name);
  if (addr == 0) {
    printf("Unknown symbol %s\n", symbol_name);
  } else {
    printf("%s: 0x%x (%d)\n", symbol_name, addr, addr);
  }
}

static bool Run(W65C02Interpreter* interpreter, const char* tail) {
  if (interpreter->pc < 0xc000) {
    printf("Run from beginning (y/n)?");
    fflush(stdout);
    char ch = fgetc(stdin);
    if (ch == 'y' || ch == 'Y') {
      W65C02Reset(interpreter);
      return true;
    }
    return false;
  }
  W65C02Reset(interpreter);
  return true;
}

static void Disassemble(W65C02Interpreter* interpreter, const char* tail) {
  int start = interpreter->pc;
  int len = 20;
  
  while (isspace(*tail)) {
    tail++;
  }
  char buf[256];
  char* p = buf;
  while (!isblank(*tail) && *tail != '\n' && *tail != '\0') {
    *p++ = *tail++;
  }
  *p = '\0';
  if (buf[0] != '\0') {
    start = (int)strtoll(buf, NULL, 16);    // Hex.
    
    while (isspace(*tail)) {
      tail++;
    }
    p = buf;
    while (!isblank(*tail) && *tail != '\n' && *tail != '\0') {
      *p++ = *tail++;
    }
    *p = '\0';
    if (buf[0] != '\0') {
      len = (int)strtoll(buf, NULL, 16);    // Hex.
    }
  }
  for (int addr = start; addr < start + len; ) {
    Breakpoint* bp = FindBreakpoint(interpreter, addr);
    if (bp != NULL) {
      BreakpointUninstall(interpreter, bp);
    }
    unsigned char* curr = &interpreter->memory[addr];
    void* next = Disassemble6502Instruction(interpreter->loader, interpreter->current_symbol, addr,
                              curr, stdout);
    addr += (unsigned char*)next - curr;
    if (bp != NULL) {
      BreakpointInstall(interpreter, bp);
    }
  }
}

static void SaveCommand(W65C02Interpreter* interpreter, const char* cmd) {
  strcpy(interpreter->last_command, cmd);
}

static void ClearSavedCommand(W65C02Interpreter* interpreter) {
  strcpy(interpreter->last_command, "");
}

static void Trace(W65C02Interpreter* interpreter, const char* tail) {
  while (isspace(*tail)) {
    tail++;
  }
  if (strcmp(tail, "on\n") == 0) {
    interpreter->trace = true;
    W65C02DisassemblePc(interpreter);
  } else {
    interpreter->trace = false;
  }
}

// The address is a real address, not a 6502 address.  Subtract interpreter->memory to
// get the 6502 address.

bool IsWatchedAddress(struct W65C02Interpreter* interpreter, WatchMode mode, const void* addr, uint8_t value) {
  uint16_t localaddr = (uint64_t)addr - (uint64_t)interpreter->memory;
  for (size_t i = 0; i < interpreter->watchpoints.length; i++) {
    Watchpoint* wp = interpreter->watchpoints.value.p[i];
    if (wp->mode != mode) {
      continue;
    }
    printf("Checking for watch %x with addr %x\n", wp->base.addr, localaddr);
    if (localaddr < wp->base.addr || localaddr > wp->base.addr + wp->span) {
      continue;
    }
    if (wp->any_value) {
      printf("Hit watchpoint %d at address 0x%x\n", wp->base.num, wp->base.addr);
      return true;
    }
    if (wp->value == value) {
      printf("Hit watchpoint %d at address 0x%x with value 0x%x\n", wp->base.num, wp->base.addr, value);
      return true;
    }
  }
  return false;
}

void DebuggerLoop(W65C02Interpreter* interpreter) {
  for (;;) {
    char buf[256];
    printf("6502> ");
    fflush(stdout);
    fgets(buf, sizeof(buf), stdin);
    const char* p = buf;
    while (isspace(*p) && *p != '\n') {
      p++;
    }
    if (*p == '\n') {
      strcpy(buf, interpreter->last_command);
      p = buf;
      while (isspace(*p) && *p != '\n') {
        p++;
      }
    }
    enum DebugCommand cmd = ParseCommand(&p);
    switch (cmd) {
      case kDbgEmpty:
        break;
      case kDbgHelp:
        Help();
        break;
      case kDbgNone:
        printf("Unknown command\n");
        ClearSavedCommand(interpreter);
        break;
      case kDbgMemory:
        DumpMemory(interpreter, p);
        ClearSavedCommand(interpreter);
        break;
      case kDbgQuit:
        exit(0);
        break;
      case kDbgRegs:
        DumpRegs(interpreter, p);
        ClearSavedCommand(interpreter);
        break;
      case kDbgSetBreak:
         SetBreakpoint(interpreter, p);
         ClearSavedCommand(interpreter);
         break;
      case kDbgSetWatch:
         SetWatchpoint(interpreter, (char*)p);
         ClearSavedCommand(interpreter);
         break;
      case kDbgStep:
        Step(interpreter, p);
        SaveCommand(interpreter, buf);
        return;
      case kDbgStepOver:
        StepOver(interpreter, p);
        SaveCommand(interpreter, buf);
        return;
      case kDbgFinish:
        Finish(interpreter);
        SaveCommand(interpreter, buf);
        return;
      case kDbgRun:
        if (Run(interpreter, p)) {
          ClearSavedCommand(interpreter);
          return;
        }
        break;
      case kDbgContinue:
        Continue(interpreter, p);
        SaveCommand(interpreter, buf);
        return;
      case kDbgDasm:
        Disassemble(interpreter, p);
        SaveCommand(interpreter, buf);
        break;
      case kDbgListBreak:
        ListBreakpoints(interpreter, p);
        break;
      case kDbgDelBreak:
        DeleteBreakpoint(interpreter, p);
        break;
     case kDbgTrace:
       Trace(interpreter, p);
       break;
      case kDbgPrint:
        Print(interpreter, p);
        break;
}
  }
}
