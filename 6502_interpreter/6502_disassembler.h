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

void* Disassemble6502Instruction(uint16_t addr, void* p, FILE* fp);

#endif /* W65C02_disassembler_h */
