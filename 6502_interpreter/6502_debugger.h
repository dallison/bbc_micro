//
//  6502_debugger.h
//  6502_interpreter
//
//  Created by David Allison on 6/4/21.
//  Copyright © 2021 David Allison. All rights reserved.
//

#ifndef _502_debugger_h
#define _502_debugger_h

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "6502_disassembler.h"

struct W65C02Interpreter;

typedef struct {
  int num;
  int addr;
} ControlPoint;

typedef struct  {
  ControlPoint base;
  char old_value;
  bool enabled;
  bool is_temp;
} Breakpoint;

typedef enum {
  kWatchWrite,
  kWatchRead,
} WatchMode;

typedef struct {
  ControlPoint base;
  int num;
  int span;      // Number of bytes beyond to check.
  WatchMode mode;     // Read or write.
  bool any_value;
  uint8_t value;
} Watchpoint;

Breakpoint* NewBreakpoint(int num, int addr, char old_value, bool temp);
void BreakpointDestruct(Breakpoint* bp);
void BreakpointDelete(Breakpoint* bp);
void BreakpointInstall(struct W65C02Interpreter* interpreter, Breakpoint* bp);
void BreakpointUninstall(struct W65C02Interpreter* interpreter, Breakpoint* bp);

Breakpoint* FindBreakpoint(struct W65C02Interpreter* interpreter, int addr);
void DebuggerLoop(struct W65C02Interpreter* interpreter);
void RemoveBreakpoint(struct W65C02Interpreter* interpreter, Breakpoint* bp);
bool IsWatchedAddress(struct W65C02Interpreter* interpreter, WatchMode mode, const void* addr, uint8_t value);
void DebuggerHexdump(struct W65C02Interpreter* interpreter, const void* addr, int size);

#endif /* _502_debugger_h */
