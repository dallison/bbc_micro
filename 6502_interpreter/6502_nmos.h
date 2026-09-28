//
//  6502_nmos.h
//  NMOS 6502 decimal mode and undocumented opcodes.
//
//  A BBC Micro has a 6502, not a 65C02. The interpreter stays a 65C02
//  unless the BBC machine asks for the NMOS behaviour. -65c02 leaves the
//  CMOS opcodes, including the davecc syscall, in place.
//

#ifndef W65C02_nmos_h
#define W65C02_nmos_h

#include "6502_interpreter.h"

#include <stdint.h>

void CpuAdd(struct W65C02Interpreter* cpu, uint8_t value);
void CpuSub(struct W65C02Interpreter* cpu, uint8_t value);

// Runs one NMOS-only opcode. Returns its cycle count, or -1 when the
// opcode is a documented 6502 instruction the CMOS core already matches.
int NmosExecute(struct W65C02Interpreter* cpu, uint8_t opcode);

#endif
