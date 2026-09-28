//
//  6502_disassembler.h
//  6502_interpreter
//
//  Created by David Allison on 5/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#ifndef W65C02_disassembler_h
#define W65C02_disassembler_h

#include <stdio.h>
#include <stdint.h>
#include "6502_machine.h"
#include "loader.h"

void* Disassemble6502Instruction(Loader* loader, SymbolScope* current_symbol, uint16_t addr, void* p, FILE* fp);
bool NamedReg(const char* name, int* value, int* size, bool* is_addr);

#endif /* W65C02_disassembler_h */
