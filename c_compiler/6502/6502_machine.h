//
//  6502_machine.h
//  c_compiler
//
//  Created by David Allison on 5/17/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#ifndef W65C02_machine_h
#define W65C02_machine_h

// 6502 Runtime
// -------------
//
// THIS IS OUT OF DATE
//
// Zero page is used to hold sets of registers:
// Single byte registers: 8 bits
// 0x00: b0
// 0x01: b1
// 0x02: b2
// 0x03: b3

// Integer/address registers: 16 bits
// 0x04/0x04: i0
// 0x06/0x07: i1
// 0x08/0x09: i2
// 0x0a/0x0b: i3
//
// Long registers: 32 bits
// 0x0c..0x0f: l0
// 0x19..0x13: l1
// 0x14..0x17: l2
// 0x18..0x1f: l3
//
// Long long registers: 64 bits
// 0x20..0x27: x0
// 0x28..0x2f: x1
// 0x30..0x37: x2
// 0x38..0x3f: x3
//
// Single precision float - 32 bits
// 0x40..0x43: f0
// 0x44..0x47: f1
// 0x48..0x4c: f2
// 0x4c..0x4f: f3
//
// Stack pointer: 16 bits - address of top of stack.
// 0x70/0x71: sp
//
// Frame pointer: 16 bits - address of bottom of stack frame.
// 0x72/0x73: fp
//
// Result address register:
// 0x74/0x75: result

// Temp registers.
// 0x76: t0
// 0x77: t1
//
// For memory pushes to stack
// 0x78, 0x79: mem_src
// 0x7a, 0x7b: mem_dest
// 0x7c, 0x7d: mem_size;

// Page 1 contains the 6502 processor stack.
//
// The program load address is 0x400 (1K) and extends up to the 48K boundary
// The addresses above 48K are ROM, containing common functions provided
// by the operating system and addresses via 16-bit vectors
//
// The main stack starts at the 48K boundary and grows down.  The heap
// starts just above the end of the program and extends up to the
// stack.  When they collide we are out of memory.

// Floating point double registers are the same as single precision regs.
#define W65C02_NUM_TEMP_B_REGS 2
#define W65C02_NUM_PRESERVED_B_REGS 6
#define W65C02_NUM_B_REGS (W65C02_NUM_TEMP_B_REGS + W65C02_NUM_PRESERVED_B_REGS)

#define W65C02_NUM_TEMP_I_REGS 4
#define W65C02_NUM_PRESERVED_I_REGS 12
#define W65C02_NUM_I_REGS (W65C02_NUM_TEMP_I_REGS + W65C02_NUM_PRESERVED_I_REGS)

#define W65C02_NUM_TEMP_L_REGS 2
#define W65C02_NUM_PRESERVED_L_REGS 6
#define W65C02_NUM_L_REGS (W65C02_NUM_TEMP_L_REGS + W65C02_NUM_PRESERVED_L_REGS)

#define W65C02_NUM_TEMP_X_REGS 1
#define W65C02_NUM_PRESERVED_X_REGS 3
#define W65C02_NUM_X_REGS (W65C02_NUM_TEMP_X_REGS + W65C02_NUM_PRESERVED_X_REGS)

#define W65C02_NUM_TEMP_F_REGS 1
#define W65C02_NUM_PRESERVED_F_REGS 3
#define W65C02_NUM_F_REGS (W65C02_NUM_TEMP_F_REGS + W65C02_NUM_PRESERVED_F_REGS)

#define W65C02_B_REG_START 0
#define W65C02_I_REG_START (W65C02_B_REG_START + W65C02_NUM_B_REGS)
#define W65C02_L_REG_START (W65C02_I_REG_START + W65C02_NUM_I_REGS*2)
#define W65C02_X_REG_START (W65C02_I_REG_START + W65C02_NUM_I_REGS*4)
#define W65C02_F_REG_START (W65C02_X_REG_START + W65C02_NUM_X_REGS*8)

#define W65C02_SP_REG (W65C02_F_REG_START + W65C02_NUM_F_REGS*4)
#define W65C02_FP_REG (W65C02_SP_REG + 2)
#define W65C02_RESULT_REG (W65C02_FP_REG + 2)
#define W65C02_T0_REG (W65C02_RESULT_REG + 2)
#define W65C02_T1_REG (W65C02_T0_REG + 1)
#define W65C02_T2_REG (W65C02_T1_REG + 1)
#define W65C02_T3_REG (W65C02_T2_REG + 1)
#define W65C02_MSRC_REG (W65C02_T3_REG + 1)
#define W65C02_MDST_REG (W65C02_MSRC_REG + 2)
#define W65C02_MSZ_REG (W65C02_MDST_REG + 2)


// The opcodes are in the first byte.  Most of them fall into the form
//   aaabbbcc
//
// Where aaa is the opcode, bbb is the addressing mode and cc is a selector.
// The valid values for cc are 00, 01 and 10.
//
// The values of aaa and cc are given for each opcode.
// The opcodes that do not fit into the standard form are given full values.

#define W65C02_OPCODE(op) k6502Opcode##op
typedef enum {
  // cc= 01
  W65C02_OPCODE(ora) = 0 << 5 | 1,
  W65C02_OPCODE(and) = 1 << 5 | 1,
  W65C02_OPCODE(eor) = 2 << 5 | 1,
  W65C02_OPCODE(adc) = 3 << 5 | 1,
  W65C02_OPCODE(sta) = 4 << 5 | 1,
  W65C02_OPCODE(lda) = 5 << 5 | 1,
  W65C02_OPCODE(cmp) = 6 << 5 | 1,
  W65C02_OPCODE(sbc) = 7 << 5 | 1,

  // cc= 10
  W65C02_OPCODE(asl) = 0 << 5 | 2,
  W65C02_OPCODE(rol) = 1 << 5 | 2,
  W65C02_OPCODE(lsr) = 2 << 5 | 2,
  W65C02_OPCODE(ror) = 3 << 5 | 2,
  W65C02_OPCODE(stx) = 4 << 5 | 2,
  W65C02_OPCODE(ldx) = 5 << 5 | 2,
  W65C02_OPCODE(dec) = 6 << 5 | 2,
  W65C02_OPCODE(inc) = 7 << 5 | 2,

  // cc= 00
  W65C02_OPCODE(bit) = 1 << 5 | 0,
  W65C02_OPCODE(jmp) = 2 << 5 | 0,
  W65C02_OPCODE(jmpr) = 3 << 5 | 0,
  W65C02_OPCODE(sty) = 4 << 5 | 0,
  W65C02_OPCODE(ldy) = 5 << 5 | 0,
  W65C02_OPCODE(cpy) = 6 << 5 | 0,
  W65C02_OPCODE(cpx) = 7 << 5 | 0,

  // Branches.
  W65C02_OPCODE(bpl) = 0x10,
  W65C02_OPCODE(bmi) = 0x30,
  W65C02_OPCODE(bvc) = 0x50,
  W65C02_OPCODE(bvs) = 0x70,
  W65C02_OPCODE(bcc) = 0x90,
  W65C02_OPCODE(bcs) = 0xb0,
  W65C02_OPCODE(bne) = 0xd0,
  W65C02_OPCODE(beq) = 0xf0,
  W65C02_OPCODE(bra) = 0x80,

  W65C02_OPCODE(brk) = 0x00,
  W65C02_OPCODE(jsr) = 0x20,
  W65C02_OPCODE(rti) = 0x40,
  W65C02_OPCODE(rts) = 0x60,

  W65C02_OPCODE(php) = 0x08,
  W65C02_OPCODE(plp) = 0x28,
  W65C02_OPCODE(pha) = 0x48,
  W65C02_OPCODE(pla) = 0x68,
  W65C02_OPCODE(dey) = 0x88,
  W65C02_OPCODE(tay) = 0xa8,
  W65C02_OPCODE(iny) = 0xc8,
  W65C02_OPCODE(inx) = 0xe8,
  W65C02_OPCODE(clc) = 0x18,
  W65C02_OPCODE(sec) = 0x38,
  W65C02_OPCODE(cli) = 0x58,
  W65C02_OPCODE(sei) = 0x78,
  W65C02_OPCODE(tya) = 0x98,
  W65C02_OPCODE(clv) = 0xb8,
  W65C02_OPCODE(cld) = 0xd8,
  W65C02_OPCODE(sed) = 0xf8,
  W65C02_OPCODE(txa) = 0x8a,
  W65C02_OPCODE(txs) = 0x9a,
  W65C02_OPCODE(tax) = 0xaa,
  W65C02_OPCODE(tsx) = 0xba,
  W65C02_OPCODE(dex) = 0xca,
  W65C02_OPCODE(nop) = 0xea,
  
  // 65C02
  W65C02_OPCODE(inca) = 0x1a,
  W65C02_OPCODE(deca) = 0x3a,
  W65C02_OPCODE(phy) = 0x5a,
  W65C02_OPCODE(ply) = 0x7a,
  W65C02_OPCODE(phx) = 0xda,
  W65C02_OPCODE(plx) = 0xfa,
} W65C02OpcodeValue;

// Addressing modes are dependent on the value of cc.
#define W65C02_ADDR_MODE(cc,x) k6502AddrMode_##cc##_##x
typedef enum {
  // cc = 01
  W65C02_ADDR_MODE(01, zpx) = 0,
  W65C02_ADDR_MODE(01, zp) = 1,
  W65C02_ADDR_MODE(01, imm) = 2,
  W65C02_ADDR_MODE(01, abs) = 3,
  W65C02_ADDR_MODE(01, zpy) = 4,
  W65C02_ADDR_MODE(01, zpxa) = 5,
  W65C02_ADDR_MODE(01, absy) = 6,
  W65C02_ADDR_MODE(01, absx) = 7,
  
  // cc = 10
  W65C02_ADDR_MODE(10, imm) = 0,
  W65C02_ADDR_MODE(10, zp) = 1,
  W65C02_ADDR_MODE(10, acc) = 2,
  W65C02_ADDR_MODE(10, abs) = 3,
  W65C02_ADDR_MODE(10, zpi) = 4,
  W65C02_ADDR_MODE(10, zpx) = 5,
  W65C02_ADDR_MODE(10, absx) = 7,

  // cc = 00
  W65C02_ADDR_MODE(00, imm) = 0,
  W65C02_ADDR_MODE(00, zp) = 1,
  W65C02_ADDR_MODE(00, abs) = 3,
  W65C02_ADDR_MODE(00, zpx) = 5,
  W65C02_ADDR_MODE(00, absx) = 7,
  W65C02_ADDR_MODE(00, bit) = 9,
} W65C02AddrMode;



#endif /* W65C02_machine_h */
