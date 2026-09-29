#ifndef BBC_SESSION_H
#define BBC_SESSION_H

#include "6502_interpreter.h"

#include <stdbool.h>

// The window hosts share the guest started from the command line.
extern W65C02Interpreter g_cpu;
extern bool g_ready;

// Parses argv, loads ROMs, and resets the machine. Returns true when a
// window should open. Otherwise *status is the process exit code and any
// guest state has already been released.
bool BbcSessionStart(int argc, char** argv, int* status);
void BbcSessionFinish(void);

#endif
