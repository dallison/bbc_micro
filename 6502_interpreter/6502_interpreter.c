//
//  6502_interpreter.c
//  6502_interpreter
//
//  Created by David Allison on 5/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include "6502_interpreter.h"
#include "elf.h"
#include "chrono_host.h"
#include "filesystem_host.h"
#include "random_host.h"
#include "loader_lifecycle.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "6502_disassembler.h"
#include "6502_devices.h"
#include "bbc_hardware.h"
#include "6502_nmos.h"
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#ifdef __MACH__
#include <mach/mach_time.h> /* mach_absolute_time */
#endif

#define W65C02_instruction_decl(x) static void Interpret_##x(W65C02Interpreter*);
#define W65C02_instruction_def(x) static void Interpret_##x(W65C02Interpreter* interpreter)
#define W65C02_instruction(x) Interpret_##x

#define W65C02_GUEST_ARGV_START 0x500
#define W65C02_GUEST_ARGV_END 0x800

typedef struct {
  void (*func)(W65C02Interpreter*);
  int bytes;
  int cycles;
} Instruction;

// Instruction function declarations, one per opcode.
W65C02_instruction_decl(brk)
W65C02_instruction_decl(ora_zp_x_indirect)
W65C02_instruction_decl(cop_zp)
W65C02_instruction_decl(ora_zp)
W65C02_instruction_decl(tsb_zp)
W65C02_instruction_decl(ora_zp)
W65C02_instruction_decl(asl_zp)
W65C02_instruction_decl(ora_zp)
W65C02_instruction_decl(php)
W65C02_instruction_decl(ora_immed)
W65C02_instruction_decl(asl_accum)
W65C02_instruction_decl(phd)
W65C02_instruction_decl(tsb_abs)
W65C02_instruction_decl(ora_abs)
W65C02_instruction_decl(asl_abs)
W65C02_instruction_decl(ora_abl)

W65C02_instruction_decl(bpl)
W65C02_instruction_decl(ora_zp_indirect_y)
W65C02_instruction_decl(ora_zp_indirect)
W65C02_instruction_decl(ora_zp_s_indirect_y)
W65C02_instruction_decl(trb_zp)
W65C02_instruction_decl(ora_zp_x)
W65C02_instruction_decl(asl_zp_x)
W65C02_instruction_decl(ora_zp_y)
W65C02_instruction_decl(clc)
W65C02_instruction_decl(ora_abs_y)
W65C02_instruction_decl(inc_a)
W65C02_instruction_decl(tcs)
W65C02_instruction_decl(trb_abs)
W65C02_instruction_decl(ora_abs_x)
W65C02_instruction_decl(asl_abs_x)
W65C02_instruction_decl(ora_abs_x)

W65C02_instruction_decl(jsr_abs)
W65C02_instruction_decl(and_zp_x_indirect)
W65C02_instruction_decl(lsr_accum)
W65C02_instruction_decl(and_zp_s)
W65C02_instruction_decl(bit_zp)
W65C02_instruction_decl(and_zp)
W65C02_instruction_decl(rol_zp)
W65C02_instruction_decl(and_zp)
W65C02_instruction_decl(plp)
W65C02_instruction_decl(and_immed)
W65C02_instruction_decl(rol_accum)
W65C02_instruction_decl(pld)
W65C02_instruction_decl(bit_abs)
W65C02_instruction_decl(and_abs)
W65C02_instruction_decl(rol_abs)
W65C02_instruction_decl(and_al)

W65C02_instruction_decl(bmi)
W65C02_instruction_decl(and_zp_indirect_y)
W65C02_instruction_decl(and_zp_indirect)
W65C02_instruction_decl(and_zp_s_indirect_y)
W65C02_instruction_decl(bit_zp_x)
W65C02_instruction_decl(and_zp_x)
W65C02_instruction_decl(rol_zp_x)
W65C02_instruction_decl(and_zp_y)
W65C02_instruction_decl(sec)
W65C02_instruction_decl(and_abs_y)
W65C02_instruction_decl(dec_accum)
W65C02_instruction_decl(tsc)
W65C02_instruction_decl(bit_abs_x)
W65C02_instruction_decl(and_abs_x)
W65C02_instruction_decl(rol_abs_x)
W65C02_instruction_decl(and_abs_x)

W65C02_instruction_decl(rti)
W65C02_instruction_decl(eor_zp_x_indirect)
W65C02_instruction_decl(wdm)
W65C02_instruction_decl(eor_zp_s)
W65C02_instruction_decl(mvp_s_zp)
W65C02_instruction_decl(eor_zp)
W65C02_instruction_decl(lsr_zp)
W65C02_instruction_decl(eor_zp)
W65C02_instruction_decl(pha)
W65C02_instruction_decl(eor_immed)
W65C02_instruction_decl(lsr_accum)
W65C02_instruction_decl(phk)
W65C02_instruction_decl(jmp_abs)
W65C02_instruction_decl(eor_abs)
W65C02_instruction_decl(lsr_abs)
W65C02_instruction_decl(eor_abs)

W65C02_instruction_decl(bvc)
W65C02_instruction_decl(eor_zp_indirect_y)
W65C02_instruction_decl(eor_zp_indirect)
W65C02_instruction_decl(eor_zp_s_indirect_y)
W65C02_instruction_decl(mvn_s_zp)
W65C02_instruction_decl(eor_zp_x)
W65C02_instruction_decl(lsr_zp_x)
W65C02_instruction_decl(eor_zp_x)
W65C02_instruction_decl(cli)
W65C02_instruction_decl(eor_abs_y)
W65C02_instruction_decl(phy)
W65C02_instruction_decl(tcd)
W65C02_instruction_decl(jmp_abs)
W65C02_instruction_decl(eor_abs_x)
W65C02_instruction_decl(lsr_abs_x)
W65C02_instruction_decl(eor_zp_x)

W65C02_instruction_decl(rts)
W65C02_instruction_decl(adc_zp_x_indirect)
W65C02_instruction_decl(per)
W65C02_instruction_decl(adc_zp_s)
W65C02_instruction_decl(stz_zp)
W65C02_instruction_decl(adc_zp)
W65C02_instruction_decl(ror_zp)
W65C02_instruction_decl(adc_zp_y)
W65C02_instruction_decl(pla)
W65C02_instruction_decl(adc_immed)
W65C02_instruction_decl(ror_accum)
W65C02_instruction_decl(rtl)
W65C02_instruction_decl(jmp_abs_indirect)
W65C02_instruction_decl(adc_abs)
W65C02_instruction_decl(ror_abs)
W65C02_instruction_decl(adc_abs)

W65C02_instruction_decl(bvs)
W65C02_instruction_decl(adc_zp_indirect_y)
W65C02_instruction_decl(adc_zp_indirect)
W65C02_instruction_decl(adc_zp_s_indirect_y)
W65C02_instruction_decl(stz_zp_x)
W65C02_instruction_decl(adc_zp_x)
W65C02_instruction_decl(ror_zp_x)
W65C02_instruction_decl(adc_zp_x)
W65C02_instruction_decl(sei)
W65C02_instruction_decl(adc_abs_y)
W65C02_instruction_decl(ply)
W65C02_instruction_decl(tdc)
W65C02_instruction_decl(jmp_abs_x_indirect)
W65C02_instruction_decl(adc_abs_x)
W65C02_instruction_decl(ror_abs_x)
W65C02_instruction_decl(adc_abs_x)

W65C02_instruction_decl(bra)
W65C02_instruction_decl(sta_zp_x_indirect)
W65C02_instruction_decl(brl)
W65C02_instruction_decl(sta_zp_s)
W65C02_instruction_decl(sty_zp)
W65C02_instruction_decl(sta_zp)
W65C02_instruction_decl(stx_zp)
W65C02_instruction_decl(sta_zp)
W65C02_instruction_decl(dey)
W65C02_instruction_decl(bit_immed)
W65C02_instruction_decl(txa)
W65C02_instruction_decl(phb)
W65C02_instruction_decl(sty_abs)
W65C02_instruction_decl(sta_abs)
W65C02_instruction_decl(stx_abs)
W65C02_instruction_decl(sta_abs)

W65C02_instruction_decl(bcc)
W65C02_instruction_decl(sta_zp_indirect_y)
W65C02_instruction_decl(sta_zp_indirect)
W65C02_instruction_decl(sta_zp_s_indirect_y)
W65C02_instruction_decl(sty_zp_x)
W65C02_instruction_decl(sta_zp_x)
W65C02_instruction_decl(stx_zp_y)
W65C02_instruction_decl(sta_abs_y)
W65C02_instruction_decl(tya)
W65C02_instruction_decl(sta_zp_y)
W65C02_instruction_decl(txs)
W65C02_instruction_decl(txy)
W65C02_instruction_decl(stz_abs)
W65C02_instruction_decl(sta_abs_x)
W65C02_instruction_decl(stz_abs_x)
W65C02_instruction_decl(sta_abs_x)

W65C02_instruction_decl(ldy_immed)
W65C02_instruction_decl(lda_zp_x_indirect)
W65C02_instruction_decl(ldx_immed)
W65C02_instruction_decl(lda_zp_s)
W65C02_instruction_decl(ldy_zp)
W65C02_instruction_decl(lda_zp)
W65C02_instruction_decl(ldx_zp)
W65C02_instruction_decl(lda_zp_indirect)
W65C02_instruction_decl(tay)
W65C02_instruction_decl(lda_immed)
W65C02_instruction_decl(tax)
W65C02_instruction_decl(plb)
W65C02_instruction_decl(ldy_abs)
W65C02_instruction_decl(lda_abs)
W65C02_instruction_decl(ldx_abs)
W65C02_instruction_decl(lda_abs)

W65C02_instruction_decl(bcs)
W65C02_instruction_decl(lda_zp_indirect_y)
W65C02_instruction_decl(lda_zp_indirect)
W65C02_instruction_decl(lda_zp_s_indirect_y)
W65C02_instruction_decl(ldy_zp_x)
W65C02_instruction_decl(lda_zp_x)
W65C02_instruction_decl(ldx_zp_y)
W65C02_instruction_decl(lda_zp_indirect_y)
W65C02_instruction_decl(clv)
W65C02_instruction_decl(lda_abs_y)
W65C02_instruction_decl(tsx)
W65C02_instruction_decl(tyx)
W65C02_instruction_decl(ldy_abs_x)
W65C02_instruction_decl(lda_abs_x)
W65C02_instruction_decl(ldx_abs_y)
W65C02_instruction_decl(lda_abs_x)

W65C02_instruction_decl(cpy_immed)
W65C02_instruction_decl(cmp_zp_x_indirect)
W65C02_instruction_decl(rep_immed)
W65C02_instruction_decl(cmp_zp_s)
W65C02_instruction_decl(cpy_zp)
W65C02_instruction_decl(cmp_zp)
W65C02_instruction_decl(dec_zp)
W65C02_instruction_decl(cmp_zp_indirect)
W65C02_instruction_decl(iny)
W65C02_instruction_decl(cmp_immed)
W65C02_instruction_decl(dex)
W65C02_instruction_decl(wal)
W65C02_instruction_decl(cpy_abs)
W65C02_instruction_decl(cmp_abs)
W65C02_instruction_decl(dec_abs)
W65C02_instruction_decl(cmp_abs)

W65C02_instruction_decl(bne)
W65C02_instruction_decl(cmp_zp_indirect_y)
W65C02_instruction_decl(cmp_zp_indirect)
W65C02_instruction_decl(cmp_zp_s_indirect_y)
W65C02_instruction_decl(pei_zp)
W65C02_instruction_decl(cmp_zp_x)
W65C02_instruction_decl(dec_zp_x)
W65C02_instruction_decl(cmp_zp_indirect_y)
W65C02_instruction_decl(cld)
W65C02_instruction_decl(cmp_abs_y)
W65C02_instruction_decl(phx)
W65C02_instruction_decl(stp)
W65C02_instruction_decl(jml_abs_indirect)
W65C02_instruction_decl(cmp_abs_x)
W65C02_instruction_decl(dec_abs_x)
W65C02_instruction_decl(cmp_abs_x)

W65C02_instruction_decl(cpx_immed)
W65C02_instruction_decl(sbc_zp_x_indirect)
W65C02_instruction_decl(sep_immed)
W65C02_instruction_decl(sbc_zp_s)
W65C02_instruction_decl(cpx_zp)
W65C02_instruction_decl(sbc_zp)
W65C02_instruction_decl(inc_zp)
W65C02_instruction_decl(sbc_zp_indirect)
W65C02_instruction_decl(inx)
W65C02_instruction_decl(sbc_immed)
W65C02_instruction_decl(nop)
W65C02_instruction_decl(xba)
W65C02_instruction_decl(cpx_abs)
W65C02_instruction_decl(sbc_abs)
W65C02_instruction_decl(inc_abs)
W65C02_instruction_decl(sbrk)

W65C02_instruction_decl(beq)
W65C02_instruction_decl(sbc_zp_indirect_y)
W65C02_instruction_decl(sbc_zp_indirect)
W65C02_instruction_decl(sbc_zp_s_indirect_y)
W65C02_instruction_decl(pea_abs)
W65C02_instruction_decl(sbc_zp_x)
W65C02_instruction_decl(inc_zp_x)
W65C02_instruction_decl(sbc_zp_indirect_y)
W65C02_instruction_decl(sed)
W65C02_instruction_decl(sbc_abs_y)
W65C02_instruction_decl(plx)
W65C02_instruction_decl(xce)
W65C02_instruction_decl(jsr_abs_x_indirect)
W65C02_instruction_decl(sbc_abs_x)
W65C02_instruction_decl(inc_abs_x)
W65C02_instruction_decl(bpt)


// Instructions array.  Ordered by opcode.
// @@@
static Instruction instructions[256] = {
  // 00 -> 0f
  {W65C02_instruction(brk), 1, 7},
  {W65C02_instruction(ora_zp_x_indirect), 2, 6},
  {W65C02_instruction(cop_zp), 2, 3},
  {W65C02_instruction(ora_zp), 2, 3},
  {W65C02_instruction(tsb_zp), 2, 5},
  {W65C02_instruction(ora_zp), 2, 3},
  {W65C02_instruction(asl_zp), 2, 5},
  {W65C02_instruction(ora_zp), 2, 3},
  {W65C02_instruction(php), 1, 3},
  {W65C02_instruction(ora_immed), 1, 2},
  {W65C02_instruction(asl_accum), 1, 2},
  {W65C02_instruction(phd), 1, 2},
  {W65C02_instruction(tsb_abs), 3, 6},
  {W65C02_instruction(ora_abs), 3, 4},
  {W65C02_instruction(asl_abs), 3, 6},
  {W65C02_instruction(ora_abl), 1, 4},
  
  // 10 -> if
  {W65C02_instruction(bpl), 1, 2},
  {W65C02_instruction(ora_zp_indirect_y), 2, 5},
  {W65C02_instruction(ora_zp_indirect), 2, 2},
  {W65C02_instruction(ora_zp_s_indirect_y), 2, 5},
  {W65C02_instruction(trb_zp), 2, 5},
  {W65C02_instruction(ora_zp_x), 2, 4},
  {W65C02_instruction(asl_zp_x), 2, 6},
  {W65C02_instruction(ora_zp_y), 2, 3},
  {W65C02_instruction(clc), 1, 2},
  {W65C02_instruction(ora_abs_y), 3, 4},
  {W65C02_instruction(inc_a), 1, 2},
  {W65C02_instruction(tcs), 1, 2},
  {W65C02_instruction(trb_abs), 3, 6},
  {W65C02_instruction(ora_abs_x), 3, 4},
  {W65C02_instruction(asl_abs_x), 3, 7},
  {W65C02_instruction(ora_abs_x), 3, 4},
  
  // 20 -> 2f
  {W65C02_instruction(jsr_abs), 3, 6},
  {W65C02_instruction(and_zp_x_indirect), 2, 6},
  {W65C02_instruction(lsr_accum), 1, 2},
  {W65C02_instruction(and_zp_s), 2, 2},
  {W65C02_instruction(bit_zp), 2, 3},
  {W65C02_instruction(and_zp), 2, 3},
  {W65C02_instruction(rol_zp), 2, 5},
  {W65C02_instruction(and_zp), 2, 2},
  {W65C02_instruction(plp), 1, 4},
  {W65C02_instruction(and_immed), 1, 2},
  {W65C02_instruction(rol_accum), 1, 2},
  {W65C02_instruction(pld), 1, 2},
  {W65C02_instruction(bit_abs), 3, 4},
  {W65C02_instruction(and_abs), 3, 4},
  {W65C02_instruction(rol_abs), 3, 6},
  {W65C02_instruction(and_al), 1, 2},
  
  // 30 -> 3f
  {W65C02_instruction(bmi), 1, 2},
  {W65C02_instruction(and_zp_indirect_y), 2, 5},
  {W65C02_instruction(and_zp_indirect), 2, 5},
  {W65C02_instruction(and_zp_s_indirect_y), 2, 5},
  {W65C02_instruction(bit_zp_x), 2, 4},
  {W65C02_instruction(and_zp_x), 2, 4},
  {W65C02_instruction(rol_zp_x), 2, 6},
  {W65C02_instruction(and_zp_y), 2, 2},
  {W65C02_instruction(sec), 1, 2},
  {W65C02_instruction(and_abs_y), 3, 4},
  {W65C02_instruction(dec_accum), 1, 2},
  {W65C02_instruction(tsc), 1, 2},
  {W65C02_instruction(bit_abs_x), 3, 4},
  {W65C02_instruction(and_abs_x), 3, 4},
  {W65C02_instruction(rol_abs_x), 3, 7},
  {W65C02_instruction(and_abs_x), 3, 4},
  
  // 40 -> 4f
  {W65C02_instruction(rti), 1, 6},
  {W65C02_instruction(eor_zp_x_indirect), 2, 6},
  {W65C02_instruction(wdm), 1, 2},
  {W65C02_instruction(eor_zp_s), 2, 2},
  {W65C02_instruction(mvp_s_zp), 2, 2},
  {W65C02_instruction(eor_zp), 2, 3},
  {W65C02_instruction(lsr_zp), 2, 5},
  {W65C02_instruction(eor_zp), 2, 3},
  {W65C02_instruction(pha), 1, 3},
  {W65C02_instruction(eor_immed), 1, 2},
  {W65C02_instruction(lsr_accum), 1, 2},
  {W65C02_instruction(phk), 1, 2},
  {W65C02_instruction(jmp_abs), 3, 3},
  {W65C02_instruction(eor_abs), 3, 4},
  {W65C02_instruction(lsr_abs), 3, 6},
  {W65C02_instruction(eor_abs), 3, 4},
  
  // 50 -> 5f
  {W65C02_instruction(bvc), 1, 2},
  {W65C02_instruction(eor_zp_indirect_y), 2, 5},
  {W65C02_instruction(eor_zp_indirect), 2, 5},
  {W65C02_instruction(eor_zp_s_indirect_y), 2, 5},
  {W65C02_instruction(mvn_s_zp), 2, 2},
  {W65C02_instruction(eor_zp_x), 2, 4},
  {W65C02_instruction(lsr_zp_x), 2, 6},
  {W65C02_instruction(eor_zp_x), 2, 4},
  {W65C02_instruction(cli), 1, 2},
  {W65C02_instruction(eor_abs_y), 3, 4},
  {W65C02_instruction(phy), 1, 2},
  {W65C02_instruction(tcd), 1, 2},
  {W65C02_instruction(jmp_abs), 3, 3},
  {W65C02_instruction(eor_abs_x), 3, 4},
  {W65C02_instruction(lsr_abs_x), 3, 7},
  {W65C02_instruction(eor_zp_x), 2, 4},
  
  // 60 -> 6f
  {W65C02_instruction(rts), 1, 6},
  {W65C02_instruction(adc_zp_x_indirect), 2, 6},
  {W65C02_instruction(per), 1, 2},
  {W65C02_instruction(adc_zp_s), 2, 2},
  {W65C02_instruction(stz_zp), 2, 2},
  {W65C02_instruction(adc_zp), 2, 3},
  {W65C02_instruction(ror_zp), 2, 5},
  {W65C02_instruction(adc_zp_y), 2, 2},
  {W65C02_instruction(pla), 1, 4},
  {W65C02_instruction(adc_immed), 1, 2},
  {W65C02_instruction(ror_accum), 1, 2},
  {W65C02_instruction(rtl), 1, 2},
  {W65C02_instruction(jmp_abs_indirect), 3, 5},
  {W65C02_instruction(adc_abs), 3, 4},
  {W65C02_instruction(ror_abs), 3, 6},
  {W65C02_instruction(adc_abs), 3, 2},
  
  // 70 -> ff
  {W65C02_instruction(bvs), 1, 2},
  {W65C02_instruction(adc_zp_indirect_y), 2, 5},
  {W65C02_instruction(adc_zp_indirect), 2, 2},
  {W65C02_instruction(adc_zp_s_indirect_y), 2, 5},
  {W65C02_instruction(stz_zp_x), 2, 4},
  {W65C02_instruction(adc_zp_x), 2, 4},
  {W65C02_instruction(ror_zp_x), 2, 6},
  {W65C02_instruction(adc_zp_x), 2, 4},
  {W65C02_instruction(sei), 1, 2},
  {W65C02_instruction(adc_abs_y), 3, 4},
  {W65C02_instruction(ply), 1, 2},
  {W65C02_instruction(tdc), 1, 2},
  {W65C02_instruction(jmp_abs_x_indirect), 3, 6},
  {W65C02_instruction(adc_abs_x), 3, 4},
  {W65C02_instruction(ror_abs_x), 3, 7},
  {W65C02_instruction(adc_abs_x), 3, 4},
  
  // 80 -> 8f
  {W65C02_instruction(bra), 1, 3},
  {W65C02_instruction(sta_zp_x_indirect), 2, 6},
  {W65C02_instruction(brl), 1, 2},
  {W65C02_instruction(sta_zp_s), 2, 3},
  {W65C02_instruction(sty_zp), 2, 3},
  {W65C02_instruction(sta_zp), 2, 3},
  {W65C02_instruction(stx_zp), 2, 3},
  {W65C02_instruction(sta_zp), 2, 3},
  {W65C02_instruction(dey), 1, 2},
  {W65C02_instruction(bit_immed), 1, 2},
  {W65C02_instruction(txa), 1, 2},
  {W65C02_instruction(phb), 1, 2},
  {W65C02_instruction(sty_abs), 3, 4},
  {W65C02_instruction(sta_abs), 3, 4},
  {W65C02_instruction(stx_abs), 3, 4},
  {W65C02_instruction(sta_abs), 3, 4},
  
  // 90 -> 9f
  {W65C02_instruction(bcc), 1, 2},
  {W65C02_instruction(sta_zp_indirect_y), 2, 6},
  {W65C02_instruction(sta_zp_indirect), 2, 2},
  {W65C02_instruction(sta_zp_s_indirect_y), 2, 5},
  {W65C02_instruction(sty_zp_x), 2, 4},
  {W65C02_instruction(sta_zp_x), 2, 4},
  {W65C02_instruction(stx_zp_y), 2, 4},
  {W65C02_instruction(sta_zp_y), 2, 4},
  {W65C02_instruction(tya), 1, 2},
  {W65C02_instruction(sta_abs_y), 2, 5},
  {W65C02_instruction(txs), 1, 2},
  {W65C02_instruction(txy), 1, 2},
  {W65C02_instruction(stz_abs), 3, 2},
  {W65C02_instruction(sta_abs_x), 3, 5},
  {W65C02_instruction(stz_abs_x), 3, 5},
  {W65C02_instruction(sta_abs_x), 3, 5},
  
  // a0 -> af
  {W65C02_instruction(ldy_immed), 1, 2},
  {W65C02_instruction(lda_zp_x_indirect), 2, 6},
  {W65C02_instruction(ldx_immed), 1, 2},
  {W65C02_instruction(lda_zp_s), 2, 3},
  {W65C02_instruction(ldy_zp), 2, 3},
  {W65C02_instruction(lda_zp), 2, 3},
  {W65C02_instruction(ldx_zp), 2, 3},
  {W65C02_instruction(lda_zp_indirect), 2, 5},
  {W65C02_instruction(tay), 1, 2},
  {W65C02_instruction(lda_immed), 1, 2},
  {W65C02_instruction(tax), 1, 2},
  {W65C02_instruction(plb), 1, 2},
  {W65C02_instruction(ldy_abs), 3, 4},
  {W65C02_instruction(lda_abs), 3, 4},
  {W65C02_instruction(ldx_abs), 3, 4},
  {W65C02_instruction(lda_abs), 3, 4},
  
  // b0 -> bf
  {W65C02_instruction(bcs), 1, 2},
  {W65C02_instruction(lda_zp_indirect_y), 2, 5},
  {W65C02_instruction(lda_zp_indirect), 2, 5},
  {W65C02_instruction(lda_zp_s_indirect_y), 2, 5},
  {W65C02_instruction(ldy_zp_x), 2, 4},
  {W65C02_instruction(lda_zp_x), 2, 4},
  {W65C02_instruction(ldx_zp_y), 2, 4},
  {W65C02_instruction(lda_zp_indirect_y), 2, 5},
  {W65C02_instruction(clv), 1, 2},
  {W65C02_instruction(lda_abs_y), 3, 4},
  {W65C02_instruction(tsx), 1, 2},
  {W65C02_instruction(tyx), 1, 2},
  {W65C02_instruction(ldy_abs_x), 3, 4},
  {W65C02_instruction(lda_abs_x), 3, 4},
  {W65C02_instruction(ldx_abs_y), 3, 4},
  {W65C02_instruction(lda_abs_x), 3, 4},
  
  // c0 -> cf
  {W65C02_instruction(cpy_immed), 1, 2},
  {W65C02_instruction(cmp_zp_x_indirect), 2, 6},
  {W65C02_instruction(rep_immed), 1, 2},
  {W65C02_instruction(cmp_zp_s), 2, 2},
  {W65C02_instruction(cpy_zp), 2, 3},
  {W65C02_instruction(cmp_zp), 2, 3},
  {W65C02_instruction(dec_zp), 2, 5},
  {W65C02_instruction(cmp_zp_indirect), 2, 5},
  {W65C02_instruction(iny), 1, 2},
  {W65C02_instruction(cmp_immed), 1, 2},
  {W65C02_instruction(dex), 1, 2},
  {W65C02_instruction(wal), 1, 2},
  {W65C02_instruction(cpy_abs), 3, 4},
  {W65C02_instruction(cmp_abs), 3, 4},
  {W65C02_instruction(dec_abs), 3, 6},
  {W65C02_instruction(cmp_abs), 3, 4},
  
  // d0 -> df
  {W65C02_instruction(bne), 1, 2},
  {W65C02_instruction(cmp_zp_indirect_y), 2, 5},
  {W65C02_instruction(cmp_zp_indirect), 2, 5},
  {W65C02_instruction(cmp_zp_s_indirect_y), 2, 5},
  {W65C02_instruction(pei_zp), 2, 2},
  {W65C02_instruction(cmp_zp_x), 2, 4},
  {W65C02_instruction(dec_zp_x), 2, 6},
  {W65C02_instruction(cmp_zp_indirect_y), 2, 5},
  {W65C02_instruction(cld), 1, 2},
  {W65C02_instruction(cmp_abs_y), 3, 4},
  {W65C02_instruction(phx), 1, 2},
  {W65C02_instruction(stp), 1, 2},
  {W65C02_instruction(jml_abs_indirect), 3, 2},
  {W65C02_instruction(cmp_abs_x), 3, 4},
  {W65C02_instruction(dec_abs_x), 3, 7},
  {W65C02_instruction(cmp_abs_x), 3, 4},
  
  // e0 -> ef
  {W65C02_instruction(cpx_immed), 1, 2},
  {W65C02_instruction(sbc_zp_x_indirect), 2, 6},
  {W65C02_instruction(sep_immed), 1, 2},
  {W65C02_instruction(sbc_zp_s), 2, 3},
  {W65C02_instruction(cpx_zp), 2, 3},
  {W65C02_instruction(sbc_zp), 2, 3},
  {W65C02_instruction(inc_zp), 2, 5},
  {W65C02_instruction(sbc_zp_indirect), 2, 5},
  {W65C02_instruction(inx), 1, 2},
  {W65C02_instruction(sbc_immed), 1, 2},
  {W65C02_instruction(nop), 1, 2},
  {W65C02_instruction(xba), 1, 2},
  {W65C02_instruction(cpx_abs), 3, 4},
  {W65C02_instruction(sbc_abs), 3, 4},
  {W65C02_instruction(inc_abs), 3, 6},
  {W65C02_instruction(sbrk), 1, 2},
  
  // f0 -> ff
  {W65C02_instruction(beq), 1, 2},
  {W65C02_instruction(sbc_zp_indirect_y), 2, 5},
  {W65C02_instruction(sbc_zp_indirect), 2, 5},
  {W65C02_instruction(sbc_zp_s_indirect_y), 2, 5},
  {W65C02_instruction(pea_abs), 3, 2},
  {W65C02_instruction(sbc_zp_x), 2, 4},
  {W65C02_instruction(inc_zp_x), 2, 6},
  {W65C02_instruction(sbc_zp_indirect_y), 2, 5},
  {W65C02_instruction(sed), 1, 2},
  {W65C02_instruction(sbc_abs_y), 3, 4},
  {W65C02_instruction(plx), 1, 2},
  {W65C02_instruction(xce), 1, 2},
  {W65C02_instruction(jsr_abs_x_indirect), 3, 2},
  {W65C02_instruction(sbc_abs_x), 3, 4},
  {W65C02_instruction(inc_abs_x), 3, 7},
  {W65C02_instruction(bpt), 1, 2},
};

void W65C02InterpreterInit(W65C02Interpreter* interpreter, bool debug,
                           bool cycle_accurate, bool trace, const char* rom_filename) {
  memset(interpreter, 0, sizeof(W65C02Interpreter));
  interpreter->debug = debug;
  interpreter->current_bp = NULL;
  interpreter->next_bp_num = 1;
  interpreter->trace = trace;
  StringInit(&interpreter->rom_filename, rom_filename);
  interpreter->cycle_accurate = cycle_accurate;
  VectorInit(&interpreter->breakpoints);
  VectorInit(&interpreter->watchpoints);

  VectorInit(&interpreter->devices);
  VectorAppend(&interpreter->devices, ConsoleInit());
  interpreter->bbc_initial_mode = -1;
  for (int i = 0; i < W65C02_MAX_OPEN_FILES; i++) {
    interpreter->open_files[i] = -1;
  }
  interpreter->open_files[0] = STDIN_FILENO;
  interpreter->open_files[1] = STDOUT_FILENO;
  interpreter->open_files[2] = STDERR_FILENO;
}

void W65C02InterpreterUseBbc(W65C02Interpreter* interpreter, int initial_mode,
                             const char* screen_ppm, const char* os_path) {
  size_t i;
  for (i = 0; i < interpreter->devices.length; i++) {
    free(interpreter->devices.value.p[i]);
  }
  VectorClear(&interpreter->devices);
  if (interpreter->bbc != NULL) {
    BbcMachineDestroy(interpreter->bbc);
  }
  interpreter->bbc = BbcMachineCreate();
  interpreter->bbc_initial_mode = initial_mode;
  interpreter->bbc_screen_path = screen_ppm;
  interpreter->bbc_os_path = os_path;
  if (interpreter->bbc != NULL) {
    Device* sheila = BbcIoDeviceCreate(interpreter->bbc);
    if (sheila != NULL) {
      VectorAppend(&interpreter->devices, sheila);
    }
  }
}

bool W65C02InterpreterBbcLoadRom(W65C02Interpreter* interpreter, int slot,
                                 const char* path) {
  if (interpreter->bbc == NULL || path == NULL) {
    return false;
  }
  return BbcMachineLoadSideways(interpreter->bbc, slot, path);
}

bool W65C02InterpreterBbcLoadDisc(W65C02Interpreter* interpreter, int drive,
                                  const char* path) {
  if (interpreter->bbc == NULL || path == NULL) {
    return false;
  }
  return BbcMachineLoadDisc(interpreter->bbc, drive, path);
}

void W65C02InterpreterBbcSetFdc(W65C02Interpreter* interpreter, int kind) {
  if (interpreter->bbc != NULL) {
    BbcMachineSetFdc(interpreter->bbc, kind);
  }
}

Device* PollForDevice(W65C02Interpreter* interpreter, uint8_t* addr) {
  for (size_t i = 0; i < interpreter->devices.length; i++) {
    Device* dev = interpreter->devices.value.p[i];
    if (dev->claim(dev, addr - interpreter->memory)) {
      return dev;
    }
  }
  return NULL;
}

#if 0
// These are used when there is no ROM.
// Reset handler, invoked on 6502 reset.
static char reset_handler[] = {
  // We have the entry address in 0,1. Jump indirect via 0x00.
  0x78,              // SEI
  0xa2, 0xff,        // LDX #0xff
  0x9a,              // TXS
  0x58,              // CLI
  0x6c, 0x00,        // JMP (0x00)
};

// 6502 instructions executed on IRQ or BRK
// Entered with interrupt flag set.  On stack we have
// s+1: P
// s+2: PC lo
// s+3: PC hi
// Use RTI to return.
// Uses 0xfd, 0xfe and 0xff as scratch.
// Jumps to BRK vector at 0xfd00
static char irq_handler[] = {
  0x85, 0xfd,         // STA $fd
  0x68,               // PLA
  0x48,               // PHA
  0x29, 0x10,         // AND #$10
  0xd0, 0x03,         // BNE $060b
  0xa5, 0xfd,         // LDA $fd
  0x40,               // RTI
  0x8a,               // TXA
  0x48,               // PHA
  0xba,               // TSX
  0xbd, 0x03, 0x01,   // LDA $0103,X
  0xd8,               // CLD
  0x38,               // SEC
  0xe9, 0x01,         // SBC #$01
  0x85, 0xfe,         // STA $fe
  0xbd, 0x04, 0x01,   // LDA $0104,X
  0xe9, 0x00,         // SBC #$00
  0x85, 0xff,         // STA $ff
  0xb2, 0xfe,         // LDA ($fe)
  0x20, 0x00, 0xfd,   // JSR 0xfd00
  0x68,               // PLA
  0xaa,               // TAX
  0xa5, 0xfd,         // LDA $fd
  0x40,               // RTI
};
#endif


// NOTE: keep this in step with the actual SP offset in zero page.
#define REG_SP 0x78
#define REG_I0 0x08

#define W65C02_INT_EXIT 1
#define W65C02_INT_EXIT_CLEAN 22
#define W65C02_GUEST_CALL_RETURN 0x0002
#define W65C02_GUEST_STACK_BOTTOM 0xc000
#define W65C02_REG_SP REG_SP
#define W65C02_INT_OPEN 2
#define W65C02_INT_CLOSE 3
#define W65C02_INT_WRITE 4
#define W65C02_INT_READ 5
#define W65C02_INT_LSEEK 7
#define W65C02_INT_ABORT 8
#define W65C02_INT_TIME 13
#define W65C02_INT_CLOCK 14
#define W65C02_INT_MONOTONIC_TIME 24
#define W65C02_INT_REALTIME_TIME 30
#define W65C02_INT_FS_STATUS 31
#define W65C02_INT_FS_OPEN_DIRECTORY 32
#define W65C02_INT_FS_READ_DIRECTORY 33
#define W65C02_INT_FS_CLOSE_DIRECTORY 34
#define W65C02_INT_FS_CREATE_DIRECTORY 35
#define W65C02_INT_FS_REMOVE 36
#define W65C02_INT_FS_RENAME 37
#define W65C02_INT_FS_CURRENT_PATH 38
#define W65C02_INT_FS_SET_CURRENT_PATH 39
#define W65C02_INT_FS_READ_SYMLINK 40
#define W65C02_INT_FS_CREATE_SYMLINK 41
#define W65C02_INT_FS_CREATE_HARD_LINK 42
#define W65C02_INT_FS_SET_PERMISSIONS 43
#define W65C02_INT_FS_RESIZE 44
#define W65C02_INT_FS_SET_MODIFICATION_TIME 45
#define W65C02_INT_FS_SPACE 46
#define W65C02_INT_FS_COPY_FILE 47
#define W65C02_INT_FS_CANONICAL 48
#define W65C02_INT_TZDB_VERSION 49
#define W65C02_INT_TZDB_GENERATION 50
#define W65C02_INT_TZDB_RELOAD 51
#define W65C02_INT_TZDB_CURRENT_ZONE 52
#define W65C02_INT_TZDB_ZONE_COUNT 53
#define W65C02_INT_TZDB_ZONE_NAME 54
#define W65C02_INT_TZDB_LOCATE_ZONE 55
#define W65C02_INT_TZDB_SYS_INFO 56
#define W65C02_INT_TZDB_LOCAL_INFO 57
#define W65C02_INT_TZDB_LEAP_COUNT 58
#define W65C02_INT_TZDB_LEAP_INFO 59
#define W65C02_INT_RANDOM_BYTES 60

static int Open(W65C02Interpreter* interpreter, const char* filename, int flags, int mode) {
  int index = 0;
  while (index < W65C02_MAX_OPEN_FILES) {
    if (interpreter->open_files[index] == -1) {
      break;
    }
    index++;
  }
  if (index == W65C02_MAX_OPEN_FILES) {
    return -1;
  }
  int fd = open(filename, flags, mode);
  if (fd == -1) {
    return -1;
  }
  interpreter->open_files[index] = fd;
  return index;
}

static int Close(W65C02Interpreter* interpreter, int fd_index) {
  if (fd_index < 0 || fd_index >= W65C02_MAX_OPEN_FILES) {
    return -1;
  }
  if (interpreter->open_files[fd_index] == -1) {
    return -1;
  }
  int e = close(interpreter->open_files[fd_index]);
  if (e == 0) {
    interpreter->open_files[fd_index] = -1;
    return 0;
  }
  return -1;
}

static off_t Lseek(W65C02Interpreter* interpreter, int fd_index, off_t offset, int whence) {
  if (fd_index < 0 || fd_index >= W65C02_MAX_OPEN_FILES) {
    return -1;
  }
  if (interpreter->open_files[fd_index] == -1) {
    return -1;
  }
  return lseek(interpreter->open_files[fd_index], offset, whence);
}


static ssize_t Write(W65C02Interpreter* interpreter, int fd_index, const char* buffer, size_t length) {
  if (fd_index < 0 || fd_index >= W65C02_MAX_OPEN_FILES) {
    return -1;
  }
  if (interpreter->open_files[fd_index] == -1) {
    return -1;
  }
  return write(interpreter->open_files[fd_index], buffer, length);
}

static ssize_t Read(W65C02Interpreter* interpreter, int fd_index, char* buffer, size_t length) {
  if (fd_index < 0 || fd_index >= W65C02_MAX_OPEN_FILES) {
    return -1;
  }
  if (interpreter->open_files[fd_index] == -1) {
    return -1;
  }
  return read(interpreter->open_files[fd_index], buffer, length);
}

// These come from fcntl.h
#define W65C02_O_RDONLY 00000000
#define W65C02_O_WRONLY 00000001
#define W65C02_O_RDWR 00000002
#define W65C02_O_ACCMODE 00000003
#define W65C02_O_CREAT 00000100
#define W65C02_O_EXCL 00000200
#define W65C02_O_NOCTTY 00000400
#define W65C02_O_TRUNC 00001000
#define W65C02_O_APPEND 00002000
#define W65C02_O_NONBLOCK 00004000
#define W65C02_O_SYNC 00010000
#define W65C02_FASYNC 00020000
#define W65C02_O_DIRECT 00040000
#define W65C02_O_LARGEFILE 00100000
#define W65C02_O_DIRECTORY 00200000
#define W65C02_O_NOFOLLOW 00400000
#define W65C02_O_NOATIME 01000000
#define W65C02_O_NDELAY W65C02_O_NONBLOCK

#define CVTFLAG(f) if ((in_flags & W65C02_##f) != 0) out_flags |= f;

// Convert the 6502 flags into those for the OS.
static int ConvertOpenFlags(int in_flags) {
  int out_flags = 0;
  if ((in_flags & W65C02_O_ACCMODE) == W65C02_O_RDWR) {
    out_flags |= O_RDWR;
  } else {
    CVTFLAG(O_WRONLY);
  }
  CVTFLAG(O_CREAT);
  CVTFLAG(O_EXCL);
  CVTFLAG(O_NOCTTY);
  CVTFLAG(O_TRUNC);
  CVTFLAG(O_APPEND);
  CVTFLAG(O_NONBLOCK)
  CVTFLAG(O_SYNC);
  CVTFLAG(FASYNC);
#if defined(O_DIRECT)
  CVTFLAG(O_DIRECT);
#endif
#if defined(O_LARGEFILE)
  CVTFLAG(O_LARGEFILE);
#endif
  CVTFLAG(O_DIRECTORY);
  CVTFLAG(O_NOFOLLOW);
#if defined(O_NOATIME)
  CVTFLAG(O_NOATIME);
#endif

  return out_flags;
}
#undef CVTFLAG

// These are from <errno.h>.
#define W65C02_EDOM 200
#define W65C02_EILSEQ 201

#define W65C02_ENOENT          1       /* No such file or directory */
#define W65C02_ENOMEM          2       /* Out of memory */
#define W65C02_EACCES          3       /* Permission denied */
#define W65C02_ENODEV          4       /* No such device */
#define W65C02_EMFILE          5       /* Too many open files */
#define W65C02_EBUSY           6       /* Device or resource busy */
#define W65C02_EINVAL          7       /* Invalid argument */
#define W65C02_ENOSPC          8       /* No space left on device */
#define W65C02_EEXIST          9       /* File exists */
#define W65C02_EAGAIN          10      /* Try again */
#define W65C02_EIO             11      /* I/O error */
#define W65C02_EINTR           12      /* Interrupted system call */
#define W65C02_ENOSYS          13      /* Function not implemented */
#define W65C02_ESPIPE          14      /* Illegal seek */
#define W65C02_ERANGE          15      /* Range error */
#define W65C02_EBADF           16      /* Bad file number */
#define W65C02_ENOEXEC         17      /* Exec format error */
#define W65C02_EUNKNOWN        18      /* Unknown OS specific error */

#define X_ERRNO(e) W65C02_##e

int TranslateErrno(int errnum) {
  switch (errnum) {
    case EDOM: return X_ERRNO(EDOM);
    case EILSEQ: return X_ERRNO(EILSEQ);

    case ENOENT: return X_ERRNO(ENOENT);
    case ENOMEM: return X_ERRNO(ENOMEM);
    case EACCES: return X_ERRNO(EACCES);
    case ENODEV: return X_ERRNO(ENODEV);
    case EMFILE: return X_ERRNO(EMFILE);
    case EBUSY: return  X_ERRNO(EBUSY);
    case EINVAL: return X_ERRNO(EINVAL);
    case ENOSPC: return X_ERRNO(ENOSPC);
    case EEXIST: return X_ERRNO(EEXIST);
    case EAGAIN: return X_ERRNO(EAGAIN);
    case EIO: return X_ERRNO(EIO);
    case EINTR: return  X_ERRNO(EINTR);
    case ENOSYS: return X_ERRNO(ENOSYS);
    case ESPIPE: return X_ERRNO(ESPIPE);
    case ERANGE: return  X_ERRNO(ERANGE);
    case EBADF: return  X_ERRNO(EBADF);
    case ENOEXEC: return  X_ERRNO(ENOEXEC);
#ifdef EUNKNOWN
    case EUNKNOWN: return  X_ERRNO(EUNKNOWN);
#endif
    default:
      return errnum;
  }
}

#define ERRNO_ADDR 0x3d6
static void SetErrno(W65C02Interpreter* interpreter) {
  *(int16_t*)&interpreter->memory[ERRNO_ADDR] = TranslateErrno(errno);
}

typedef struct {
  uint16_t pc;
  uint8_t a;
  uint8_t x;
  uint8_t y;
  uint8_t s;
  int8_t flags;
  uint16_t guest_call_return_pc;
} W65C02SavedState;

static int StepOneInstruction(W65C02Interpreter* interpreter,
                              bool cycle_accurate);

static void W65C02SaveState(W65C02Interpreter* interpreter,
                            W65C02SavedState* saved) {
  saved->pc = interpreter->pc;
  saved->a = interpreter->a;
  saved->x = interpreter->x;
  saved->y = interpreter->y;
  saved->s = interpreter->s;
  saved->flags = interpreter->flags.value;
  saved->guest_call_return_pc = interpreter->guest_call_return_pc;
}

static void W65C02RestoreState(W65C02Interpreter* interpreter,
                               const W65C02SavedState* saved) {
  interpreter->pc = saved->pc;
  interpreter->a = saved->a;
  interpreter->x = saved->x;
  interpreter->y = saved->y;
  interpreter->s = saved->s;
  interpreter->flags.value = saved->flags;
  interpreter->guest_call_return_pc = saved->guest_call_return_pc;
}

static void W65C02SetupGuestStartupStack(W65C02Interpreter* interpreter) {
  interpreter->memory[W65C02_REG_SP] = W65C02_GUEST_STACK_BOTTOM & 0xff;
  interpreter->memory[W65C02_REG_SP + 1] = W65C02_GUEST_STACK_BOTTOM >> 8;
  interpreter->memory[W65C02_REG_SP + 2] = W65C02_GUEST_STACK_BOTTOM & 0xff;
  interpreter->memory[W65C02_REG_SP + 3] = W65C02_GUEST_STACK_BOTTOM >> 8;
  interpreter->s = 0xff;
}

bool W65C02GuestAddressExecutable(Loader* loader, uint16_t addr) {
  if (loader == NULL) {
    return false;
  }
  if (addr == 0) {
    return true;
  }
  for (size_t i = 1; i < loader->regions.length; i++) {
    Region* region = loader->regions.value.p[i];
    for (size_t section_index = 0; section_index < region->sections.length;
         section_index++) {
      ELFReaderSection* section = region->sections.value.p[section_index];
      if ((section->header->flags & SHF(execinstr)) == 0) {
        continue;
      }
      uint16_t start = (uint16_t)section->header->addr;
      uint16_t end = start + (uint16_t)section->header->size;
      if (addr >= start && addr < end) {
        return true;
      }
    }
  }
  return false;
}

static void W65C02PushGuestReturnAddress(W65C02Interpreter* interpreter,
                                         uint16_t return_pc) {
  uint16_t pushed = (uint16_t)(return_pc - 1);
  interpreter->memory[0x100 + interpreter->s--] = pushed >> 8;
  interpreter->memory[0x100 + interpreter->s--] = pushed & 0xff;
}

void W65C02GuestCallVoidFunction(W65C02Interpreter* interpreter, uint16_t fn) {
  if (interpreter == NULL || fn == 0) {
    return;
  }
  W65C02SavedState saved;
  W65C02SaveState(interpreter, &saved);
  W65C02PushGuestReturnAddress(interpreter, W65C02_GUEST_CALL_RETURN);
  interpreter->guest_call_return_pc = W65C02_GUEST_CALL_RETURN;
  interpreter->pc = fn;
  while (interpreter->pc != interpreter->guest_call_return_pc) {
    StepOneInstruction(interpreter, false);
  }
  W65C02RestoreState(interpreter, &saved);
}

static bool W65C02LifecycleCallback(void* context, LoadedDynamicLibrary* image,
                                    uint64_t function,
                                    LoaderLifecyclePhase phase) {
  (void)image;
  (void)phase;
  typedef struct {
    Loader* loader;
    W65C02Interpreter* interpreter;
  } W65C02LifecycleContext;
  W65C02LifecycleContext* ctx = context;
  if (function > 0xffff ||
      !W65C02GuestAddressExecutable(ctx->loader, (uint16_t)function)) {
    LoaderError("Function array entry 0x%llx is not executable\n",
                (unsigned long long)function);
    return false;
  }
  W65C02GuestCallVoidFunction(ctx->interpreter, (uint16_t)function);
  return true;
}

static bool RunGuestLifecyclePhase(Loader* loader,
                                   W65C02Interpreter* interpreter,
                                   LoaderLifecyclePhase phase) {
  typedef struct {
    Loader* loader;
    W65C02Interpreter* interpreter;
  } W65C02LifecycleContext;
  W65C02LifecycleContext ctx = {loader, interpreter};
  return LoaderLifecycleRunPhase(loader, loader->lifecycle, phase,
                                 W65C02LifecycleCallback, &ctx);
}

bool W65C02GuestRunInitArrays(Loader* loader,
                              W65C02Interpreter* interpreter) {
  if (interpreter->init_arrays_done) {
    return true;
  }
  if (!RunGuestLifecyclePhase(loader, interpreter, kLoaderLifecyclePreinit) ||
      !RunGuestLifecyclePhase(loader, interpreter, kLoaderLifecycleInit)) {
    return false;
  }
  interpreter->init_arrays_done = true;
  return true;
}

bool W65C02GuestRunFiniArrays(Loader* loader,
                              W65C02Interpreter* interpreter) {
  return RunGuestLifecyclePhase(loader, interpreter, kLoaderLifecycleFini);
}

static uint16_t GuestRead16(const void* pointer) {
  uint16_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

static int32_t GuestRead32(const void* pointer) {
  int32_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

static void* GuestPointer(W65C02Interpreter* interpreter, uint16_t address,
                          size_t size) {
  if (size > 0x10000u || (size_t)address + size > 0x10000u) {
    return NULL;
  }
  return interpreter->memory + address;
}

static const char* GuestString(W65C02Interpreter* interpreter,
                               uint16_t address) {
  const char* value = GuestPointer(interpreter, address, 1);
  if (value == NULL ||
      memchr(value, '\0', 0x10000u - (size_t)address) == NULL) {
    return NULL;
  }
  return value;
}

static void SetSyscallResult(W65C02Interpreter* interpreter,
                             uint16_t result_address, int64_t result) {
  int32_t narrowed = (int32_t)result;
  void* destination =
      GuestPointer(interpreter, result_address, sizeof(narrowed));
  if (destination != NULL) {
    memcpy(destination, &narrowed, sizeof(narrowed));
  }
  interpreter->x = narrowed & 0xff;
  interpreter->y = (narrowed >> 8) & 0xff;
}

static int64_t GuestClockMicroseconds(W65C02Interpreter* interpreter,
                                     uint16_t result_address,
                                     clockid_t clock_id) {
  int64_t* destination =
      GuestPointer(interpreter, result_address, sizeof(int64_t));
  struct timespec now;
  if (destination == NULL || clock_gettime(clock_id, &now) != 0) {
    return -DAVE_HOST_EINVAL;
  }
  int64_t value = (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
  memcpy(destination, &value, sizeof(value));
  return 0;
}

// This is entered from the interpreter's custom software-break instruction.
// sp+0 is the address of the four-byte long result and sp+2 starts the packed
// 65C02 arguments (all pointers and int/size_t arguments are two bytes).
static void BrkHandler(W65C02Interpreter* interpreter, int8_t code) {
  uint16_t guest_sp = GuestRead16(&interpreter->memory[REG_SP]);
  uint8_t* sp = GuestPointer(interpreter, guest_sp, 2);
  if (sp == NULL) {
    interpreter->exit_code = 1;
    interpreter->running = false;
    return;
  }
  uint16_t result_address = GuestRead16(sp);
  int64_t result = 0;

#define ARG16(offset) GuestRead16(sp + 2 + (offset))
#define ARG_PTR(offset, size) \
  GuestPointer(interpreter, ARG16(offset), (size))
#define ARG_STRING(offset) GuestString(interpreter, ARG16(offset))

  switch (code) {
    case W65C02_INT_EXIT:
      exit((int16_t)ARG16(0));
    case W65C02_INT_EXIT_CLEAN:
      interpreter->exit_code = (int16_t)ARG16(0);
      LoaderLifecycleMarkExecutableFiniComplete(interpreter->loader,
                                                interpreter->loader->lifecycle);
      interpreter->running = false;
      return;
    case W65C02_INT_ABORT:
      fprintf(stderr, "guest abort at pc=0x%04x sp=0x%04x s=0x%02x stack:",
              interpreter->pc, guest_sp, interpreter->s);
      for (int i = 1; i <= 16; ++i) {
        fprintf(stderr, " %02x",
                interpreter->memory[0x100 + (uint8_t)(interpreter->s + i)]);
      }
      fprintf(stderr, "\n");
      abort();
    case W65C02_INT_OPEN: {
      const char* filename = ARG_STRING(0);
      if (filename == NULL) {
        result = -1;
        errno = EINVAL;
      } else {
        result = Open(interpreter, filename, ConvertOpenFlags(ARG16(2)),
                      ARG16(4));
      }
      break;
    }
    case W65C02_INT_CLOSE:
      result = Close(interpreter, (int16_t)ARG16(0));
      break;
    case W65C02_INT_LSEEK:
      result = Lseek(interpreter, (int16_t)ARG16(0),
                     (off_t)GuestRead32(sp + 4), (int16_t)ARG16(6));
      break;
    case W65C02_INT_WRITE: {
      size_t length = ARG16(4);
      const char* buffer = ARG_PTR(2, length);
      result = buffer == NULL
                   ? -1
                   : Write(interpreter, (int16_t)ARG16(0), buffer, length);
      if (buffer == NULL) errno = EINVAL;
      break;
    }
    case W65C02_INT_READ: {
      size_t length = ARG16(4);
      char* buffer = ARG_PTR(2, length);
      result = buffer == NULL
                   ? -1
                   : Read(interpreter, (int16_t)ARG16(0), buffer, length);
      if (buffer == NULL) errno = EINVAL;
      break;
    }
    case W65C02_INT_TIME:
      result = (int64_t)time(NULL);
      break;
    case W65C02_INT_CLOCK: {
      struct timespec now;
      result = clock_gettime(CLOCK_MONOTONIC, &now) == 0
                   ? (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000
                   : -1;
      break;
    }
    case W65C02_INT_MONOTONIC_TIME:
      result = GuestClockMicroseconds(interpreter, ARG16(0),
                                      CLOCK_MONOTONIC);
      break;
    case W65C02_INT_REALTIME_TIME:
      result =
          GuestClockMicroseconds(interpreter, ARG16(0), CLOCK_REALTIME);
      break;
    case W65C02_INT_FS_STATUS: {
      DaveHostFilesystemStat host_result;
      DaveHostFilesystemStat* guest_result = ARG_PTR(4, sizeof(host_result));
      const char* path = ARG_STRING(0);
      if (path == NULL || guest_result == NULL) {
        result = -DAVE_HOST_EINVAL;
      } else {
        result =
            DaveHostFilesystemGetStatus(path, ARG16(2), &host_result);
        if (result == 0) {
          memcpy(guest_result, &host_result, sizeof(host_result));
        }
      }
      break;
    }
    case W65C02_INT_FS_OPEN_DIRECTORY: {
      const char* path = ARG_STRING(0);
      result = path == NULL ? -DAVE_HOST_EINVAL
                            : DaveHostFilesystemOpenDirectory(path);
      break;
    }
    case W65C02_INT_FS_READ_DIRECTORY: {
      DaveHostFilesystemDirectoryEntry host_result;
      DaveHostFilesystemDirectoryEntry* guest_result =
          ARG_PTR(2, sizeof(host_result));
      if (guest_result == NULL) {
        result = -DAVE_HOST_EINVAL;
      } else {
        result = DaveHostFilesystemReadDirectory((int16_t)ARG16(0),
                                                 &host_result);
        if (result > 0) {
          memcpy(guest_result, &host_result, sizeof(host_result));
        }
      }
      break;
    }
    case W65C02_INT_FS_CLOSE_DIRECTORY:
      result = DaveHostFilesystemCloseDirectory((int16_t)ARG16(0));
      break;
    case W65C02_INT_FS_CREATE_DIRECTORY: {
      const char* path = ARG_STRING(0);
      result = path == NULL
                   ? -DAVE_HOST_EINVAL
                   : DaveHostFilesystemCreateDirectory(path, ARG16(2));
      break;
    }
    case W65C02_INT_FS_REMOVE: {
      const char* path = ARG_STRING(0);
      result = path == NULL ? -DAVE_HOST_EINVAL
                            : DaveHostFilesystemRemove(path);
      break;
    }
    case W65C02_INT_FS_RENAME: {
      const char* old_path = ARG_STRING(0);
      const char* new_path = ARG_STRING(2);
      result = old_path == NULL || new_path == NULL
                   ? -DAVE_HOST_EINVAL
                   : DaveHostFilesystemRename(old_path, new_path);
      break;
    }
    case W65C02_INT_FS_CURRENT_PATH: {
      size_t capacity = ARG16(2);
      char* buffer = ARG_PTR(0, capacity);
      result = buffer == NULL ? -DAVE_HOST_EINVAL
                              : DaveHostFilesystemCurrentPath(buffer,
                                                              capacity);
      break;
    }
    case W65C02_INT_FS_SET_CURRENT_PATH: {
      const char* path = ARG_STRING(0);
      result = path == NULL ? -DAVE_HOST_EINVAL
                            : DaveHostFilesystemSetCurrentPath(path);
      break;
    }
    case W65C02_INT_FS_READ_SYMLINK:
    case W65C02_INT_FS_CANONICAL: {
      const char* path = ARG_STRING(0);
      size_t capacity = ARG16(4);
      char* buffer = ARG_PTR(2, capacity);
      if (path == NULL || buffer == NULL) {
        result = -DAVE_HOST_EINVAL;
      } else if (code == W65C02_INT_FS_READ_SYMLINK) {
        result = DaveHostFilesystemReadSymlink(path, buffer, capacity);
      } else {
        result = DaveHostFilesystemCanonical(path, buffer, capacity);
      }
      break;
    }
    case W65C02_INT_FS_CREATE_SYMLINK:
    case W65C02_INT_FS_CREATE_HARD_LINK: {
      const char* target = ARG_STRING(0);
      const char* link = ARG_STRING(2);
      if (target == NULL || link == NULL) {
        result = -DAVE_HOST_EINVAL;
      } else if (code == W65C02_INT_FS_CREATE_SYMLINK) {
        result = DaveHostFilesystemCreateSymlink(target, link);
      } else {
        result = DaveHostFilesystemCreateHardLink(target, link);
      }
      break;
    }
    case W65C02_INT_FS_SET_PERMISSIONS: {
      const char* path = ARG_STRING(0);
      result = path == NULL
                   ? -DAVE_HOST_EINVAL
                   : DaveHostFilesystemSetPermissions(path, ARG16(2),
                                                      ARG16(4));
      break;
    }
    case W65C02_INT_FS_RESIZE: {
      const char* path = ARG_STRING(0);
      const uint64_t* size = ARG_PTR(2, sizeof(uint64_t));
      uint64_t value;
      if (path == NULL || size == NULL) {
        result = -DAVE_HOST_EINVAL;
      } else {
        memcpy(&value, size, sizeof(value));
        result = DaveHostFilesystemResize(path, value);
      }
      break;
    }
    case W65C02_INT_FS_SET_MODIFICATION_TIME: {
      const char* path = ARG_STRING(0);
      const int64_t* nanoseconds = ARG_PTR(2, sizeof(int64_t));
      int64_t value;
      if (path == NULL || nanoseconds == NULL) {
        result = -DAVE_HOST_EINVAL;
      } else {
        memcpy(&value, nanoseconds, sizeof(value));
        result = DaveHostFilesystemSetModificationTime(path, value);
      }
      break;
    }
    case W65C02_INT_FS_SPACE: {
      DaveHostFilesystemSpace host_result;
      DaveHostFilesystemSpace* guest_result =
          ARG_PTR(2, sizeof(host_result));
      const char* path = ARG_STRING(0);
      if (path == NULL || guest_result == NULL) {
        result = -DAVE_HOST_EINVAL;
      } else {
        result = DaveHostFilesystemQuerySpace(path, &host_result);
        if (result == 0) {
          memcpy(guest_result, &host_result, sizeof(host_result));
        }
      }
      break;
    }
    case W65C02_INT_FS_COPY_FILE: {
      const char* source = ARG_STRING(0);
      const char* destination = ARG_STRING(2);
      result = source == NULL || destination == NULL
                   ? -DAVE_HOST_EINVAL
                   : DaveHostFilesystemCopyFile(source, destination,
                                                ARG16(4));
      break;
    }
    case W65C02_INT_TZDB_VERSION: {
      size_t capacity = ARG16(2);
      char* buffer = ARG_PTR(0, capacity);
      result = buffer == NULL ? -DAVE_HOST_EINVAL
                              : DaveHostChronoTzdbVersion(buffer, capacity);
      break;
    }
    case W65C02_INT_TZDB_GENERATION: {
      uint64_t* generation = ARG_PTR(0, sizeof(uint64_t));
      result = generation == NULL ? -DAVE_HOST_EINVAL
                                  : DaveHostChronoGeneration(generation);
      break;
    }
    case W65C02_INT_TZDB_RELOAD: {
      uint64_t* generation = ARG_PTR(0, sizeof(uint64_t));
      result = DaveHostChronoReload(generation);
      break;
    }
    case W65C02_INT_TZDB_CURRENT_ZONE: {
      size_t capacity = ARG16(2);
      char* buffer = ARG_PTR(0, capacity);
      result = buffer == NULL ? -DAVE_HOST_EINVAL
                              : DaveHostChronoCurrentZone(buffer, capacity);
      break;
    }
    case W65C02_INT_TZDB_ZONE_COUNT: {
      uint32_t* count = ARG_PTR(0, sizeof(uint32_t));
      result = count == NULL ? -DAVE_HOST_EINVAL
                             : DaveHostChronoZoneCount(count);
      break;
    }
    case W65C02_INT_TZDB_ZONE_NAME: {
      size_t capacity = ARG16(4);
      char* buffer = ARG_PTR(2, capacity);
      result = buffer == NULL
                   ? -DAVE_HOST_EINVAL
                   : DaveHostChronoZoneName((uint32_t)ARG16(0), buffer, capacity);
      break;
    }
    case W65C02_INT_TZDB_LOCATE_ZONE: {
      const char* name = ARG_STRING(0);
      size_t capacity = ARG16(4);
      char* buffer = ARG_PTR(2, capacity);
      uint32_t* index = ARG_PTR(6, sizeof(uint32_t));
      result = name == NULL || buffer == NULL || index == NULL
                   ? -DAVE_HOST_EINVAL
                   : DaveHostChronoLocateZone(name, buffer, capacity, index);
      break;
    }
    case W65C02_INT_TZDB_SYS_INFO: {
      const char* zone = ARG_STRING(0);
      int64_t* seconds = (int64_t*)ARG_PTR(2, 8);
      DaveHostChronoSysInfoWire* info =
          (DaveHostChronoSysInfoWire*)ARG_PTR(4, sizeof(DaveHostChronoSysInfoWire));
      size_t capacity = ARG16(8);
      char* abbrev = ARG_PTR(6, capacity);
      if (zone == NULL || seconds == NULL || info == NULL || abbrev == NULL) {
        result = -DAVE_HOST_EINVAL;
      } else {
        int64_t value = 0;
        memcpy(&value, seconds, sizeof(value));
        result = DaveHostChronoSysInfo(zone, value, info, abbrev, capacity);
      }
      break;
    }
    case W65C02_INT_TZDB_LOCAL_INFO: {
      const char* zone = ARG_STRING(0);
      int64_t* seconds = (int64_t*)ARG_PTR(2, 8);
      DaveHostChronoLocalInfoWire* info =
          (DaveHostChronoLocalInfoWire*)ARG_PTR(4, sizeof(DaveHostChronoLocalInfoWire));
      size_t capacity = ARG16(8);
      char* abbrev = ARG_PTR(6, capacity * 2);
      if (zone == NULL || seconds == NULL || info == NULL || abbrev == NULL ||
          capacity == 0) {
        result = -DAVE_HOST_EINVAL;
      } else {
        int64_t value = 0;
        memcpy(&value, seconds, sizeof(value));
        result = DaveHostChronoLocalInfo(zone, value, info, abbrev, capacity,
                                         abbrev + capacity, capacity);
      }
      break;
    }
    case W65C02_INT_TZDB_LEAP_COUNT: {
      uint32_t* count = ARG_PTR(0, sizeof(uint32_t));
      result = count == NULL ? -DAVE_HOST_EINVAL
                             : DaveHostChronoLeapCount(count);
      break;
    }
    case W65C02_INT_TZDB_LEAP_INFO: {
      DaveHostChronoLeapSecond* leap =
          ARG_PTR(2, sizeof(DaveHostChronoLeapSecond));
      result = leap == NULL
                   ? -DAVE_HOST_EINVAL
                   : DaveHostChronoLeapInfo((uint32_t)ARG16(0), leap);
      break;
    }
    case W65C02_INT_RANDOM_BYTES: {
      size_t size = ARG16(2);
      void* buffer = size == 0 ? NULL : ARG_PTR(0, size);
      result = size != 0 && buffer == NULL
                   ? -DAVE_HOST_EINVAL
                   : DaveHostRandomBytes(buffer, size);
      break;
    }
    default:
      result = -DAVE_HOST_ENOSYS;
      break;
  }

  if (result == -1) {
    SetErrno(interpreter);
  }
  SetSyscallResult(interpreter, result_address, result);

#undef ARG16
#undef ARG_PTR
#undef ARG_STRING
}


void W65C02DisassemblePc(W65C02Interpreter* interpreter) {
  interpreter->current_symbol =
      LoaderFindSymbolAndCacheResult(interpreter->loader, interpreter->pc);
  Breakpoint* bp = FindBreakpoint(interpreter, interpreter->pc);
  if (bp != NULL) {
    BreakpointUninstall(interpreter, bp);
  }
  Disassemble6502Instruction(interpreter->loader, interpreter->current_symbol, interpreter->pc,
                            &interpreter->memory[interpreter->pc], stdout);
  if (bp != NULL) {
    BreakpointInstall(interpreter, bp);
  }
}

// 2 MHz = 500 ns per cycle.
#define CPU_CYCLE_NS 500

#ifdef __MACH__
static uint64_t TicksPerCycle(void) {
  static mach_timebase_info_data_t info;
  static uint64_t ticks = 0;
  if (ticks == 0) {
    mach_timebase_info(&info);
    ticks = ((uint64_t)CPU_CYCLE_NS * info.denom + info.numer / 2) / info.numer;
    if (ticks == 0) {
      ticks = 1;
    }
  }
  return ticks;
}
#endif

// A short stretch with interrupts on and almost no stores is the keyboard
// wait, a delay loop, or a poll of the CRTC or system VIA. A disc poll reads
// the controller instead, and a memory fill stores constantly: those run ahead.
static void UpdatePace(W65C02Interpreter* interpreter, int cycles) {
  bool waiting;
  if (interpreter->bbc == NULL || interpreter->bbc_os_path == NULL) {
    interpreter->turbo = false;
    return;
  }
  interpreter->pace_cycles += cycles;
  if (interpreter->pace_cycles < 8192) {
    return;
  }
  waiting = !interpreter->flags.bits.i && interpreter->pace_stores < 48 &&
            (interpreter->pace_watch > 0 || interpreter->pace_sheila == 0);
  // A cursor blink or one disc poll looks like work for a single window.
  // Drop to real time immediately, and only run ahead after the guest has
  // stayed busy, so the frame size does not flap.
  if (waiting || BbcMachineSounding(interpreter->bbc)) {
    interpreter->turbo = false;
    interpreter->pace_busy = 0;
  } else if (interpreter->pace_busy < 4) {
    interpreter->pace_busy++;
  } else {
    interpreter->turbo = true;
  }
  interpreter->pace_cycles = 0;
  interpreter->pace_stores = 0;
  interpreter->pace_sheila = 0;
  interpreter->pace_watch = 0;
}

void W65C02CatchUp(W65C02Interpreter* interpreter) {
  int owed;
  if (interpreter == NULL || interpreter->turbo_owed <= 0 || interpreter->bbc == NULL) {
    return;
  }
  owed = interpreter->turbo_owed;
  interpreter->turbo_owed = 0;
  BbcMachineSetFast(interpreter->bbc, interpreter->turbo && interpreter->bbc_os_path != NULL);
  BbcMachineAdvance(interpreter->bbc, owed);
}

// Keep the guest at 2 MHz without sleeping on every opcode. The wait is
// banked until about 1 ms of guest time has accumulated, then the thread
// sleeps until that deadline. Turbo skips the sleep.
static void ChargeCycles(W65C02Interpreter* interpreter, bool cycle_accurate, int cycles) {
  uint64_t now;
  uint64_t target;
  int step_cycles = cycles;
  if (interpreter->bbc != NULL) {
    bool fast = interpreter->turbo && interpreter->bbc_os_path != NULL;
    if (fast && BbcMachineSounding(interpreter->bbc)) {
      fast = false;
      interpreter->turbo = false;
      interpreter->pace_busy = 0;
    }
    // The per-instruction hardware update is most of the cost of a memory
    // fill. While running ahead, apply it a block at a time, but never step
    // past a disc byte: the controller raises NMI when the delay expires.
    int due = BbcMachineDiscHorizon(interpreter->bbc);
    if (interpreter->turbo_owed > 0) {
      if (due <= interpreter->turbo_owed) {
        due = 0;
      } else {
        due -= interpreter->turbo_owed;
      }
    }
    if (fast && due > step_cycles && interpreter->turbo_owed + step_cycles < 1024) {
      interpreter->turbo_owed += step_cycles;
      UpdatePace(interpreter, step_cycles);
      return;
    }
    if (interpreter->turbo_owed > 0) {
      cycles += interpreter->turbo_owed;
      interpreter->turbo_owed = 0;
    }
    BbcMachineSetFast(interpreter->bbc, fast);
    BbcMachineAdvance(interpreter->bbc, cycles);
  }
  UpdatePace(interpreter, step_cycles);
  if (interpreter->turbo || !cycle_accurate || !interpreter->cycle_accurate || step_cycles <= 0) {
    return;
  }
  interpreter->owed_cycles += step_cycles;
  if (interpreter->owed_cycles < 2000) {
    return;
  }
#ifdef __MACH__
  now = mach_absolute_time();
#else
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
#endif
  if (interpreter->cycle_anchor == 0) {
    interpreter->cycle_anchor = now;
    interpreter->cycle_origin = 0;
  }
  interpreter->cycle_origin += (uint64_t)interpreter->owed_cycles;
  interpreter->owed_cycles = 0;
#ifdef __MACH__
  target = interpreter->cycle_anchor + interpreter->cycle_origin * TicksPerCycle();
  if (target > now) {
    mach_wait_until(target);
  } else if (now - target > 50000000ull * TicksPerCycle() / (uint64_t)CPU_CYCLE_NS) {
    interpreter->cycle_anchor = now;
    interpreter->cycle_origin = 0;
  }
#else
  target = interpreter->cycle_anchor + interpreter->cycle_origin * (uint64_t)CPU_CYCLE_NS;
  if (target > now) {
    uint64_t wait = target - now;
    struct timespec delay;
    if (wait > 20000000ull) {
      wait = 20000000ull;
    }
    delay.tv_sec = (time_t)(wait / 1000000000ull);
    delay.tv_nsec = (long)(wait % 1000000000ull);
    nanosleep(&delay, NULL);
  } else if (now - target > 50000000ull) {
    interpreter->cycle_anchor = now;
    interpreter->cycle_origin = 0;
  }
#endif
}


static void Sc12LeaveDecimal(W65C02Interpreter* interpreter) {
  // An interrupt or BRK clears D on the 65SC12. The stacked status still
  // has the flag as it was.
  if (interpreter->sc12) {
    interpreter->flags.bits.d = 0;
  }
}

static void W65C02TakeNmi(W65C02Interpreter* interpreter) {
  uint16_t pc = interpreter->pc;
  uint8_t status = (uint8_t)((interpreter->flags.value & ~(1 << 4)) | (1 << 5));
  interpreter->memory[0x100 + interpreter->s--] = (uint8_t)(pc >> 8);
  interpreter->memory[0x100 + interpreter->s--] = (uint8_t)(pc & 0xff);
  interpreter->memory[0x100 + interpreter->s--] = status;
  interpreter->flags.bits.i = 1;
  Sc12LeaveDecimal(interpreter);
  interpreter->in_irq++;
  interpreter->pc = (uint16_t)(interpreter->memory[0xfffa] |
                               (interpreter->memory[0xfffb] << 8));
}

static void W65C02TakeIrq(W65C02Interpreter* interpreter) {
  uint16_t pc = interpreter->pc;
  uint8_t status = (uint8_t)((interpreter->flags.value & ~(1 << 4)) | (1 << 5));
  interpreter->memory[0x100 + interpreter->s--] = (uint8_t)(pc >> 8);
  interpreter->memory[0x100 + interpreter->s--] = (uint8_t)(pc & 0xff);
  interpreter->memory[0x100 + interpreter->s--] = status;
  interpreter->flags.bits.i = 1;
  Sc12LeaveDecimal(interpreter);
  interpreter->in_irq++;
  interpreter->pc = (uint16_t)(interpreter->memory[0xfffe] |
                               (interpreter->memory[0xffff] << 8));
}

void W65C02NoteStore(W65C02Interpreter* cpu, uint16_t addr) {
  if (cpu->in_irq || (addr >= 0x100 && addr <= 0x1ff)) {
    return;
  }
  if (cpu->pace_stores < 100000) {
    cpu->pace_stores++;
  }
}

void W65C02NoteRead(W65C02Interpreter* cpu, uint16_t addr) {
  uint8_t page;
  if (cpu->in_irq || addr < 0xfc00 || addr > 0xfeff) {
    return;
  }
  cpu->pace_sheila++;
  page = (uint8_t)(addr & 0xff);
  // CRTC, video ULA, and the system VIA. The Master's WD1770 at &FE24 is
  // not one of these: polling it should run ahead with the CPU.
  if (page < 0x20 || (page >= 0x40 && page < 0x60)) {
    cpu->pace_watch++;
  } else if (page >= 0x20 && page < 0x30 &&
             !(cpu->bbc != NULL && BbcMachineIsMaster(cpu->bbc) && page >= 0x24)) {
    cpu->pace_watch++;
  }
}

static inline bool W65C02MemIsIo(const W65C02Interpreter* interpreter, const uint8_t* addr) {
  uintptr_t off = (uintptr_t)(addr - interpreter->memory);
  if (off > 0xffff) {
    return false;
  }
  if (interpreter->bbc != NULL) {
    return off >= 0xfc00 && off <= 0xfeff;
  }
  return off >= W65C02_IO_START && off <= W65C02_IO_END;
}

static int StepOneInstruction(W65C02Interpreter* interpreter, bool cycle_accurate) {
  int cycles;
  interpreter->extra_cycles = 0;
  if (interpreter->jammed) {
    ChargeCycles(interpreter, cycle_accurate, 1);
    return 1;
  }
  if (interpreter->turbo_owed > 0 && interpreter->bbc != NULL &&
      (BbcMachineNmiPending(interpreter->bbc) ||
       (!interpreter->flags.bits.i && BbcMachineIrqPending(interpreter->bbc)))) {
    W65C02CatchUp(interpreter);
  }
  if (interpreter->bbc != NULL && BbcMachineNmiPending(interpreter->bbc)) {
    BbcMachineClearNmi(interpreter->bbc);
    W65C02TakeNmi(interpreter);
    ChargeCycles(interpreter, cycle_accurate, 7);
    return 7;
  }
  if (interpreter->bbc != NULL && !interpreter->flags.bits.i &&
      BbcMachineIrqPending(interpreter->bbc)) {
    W65C02TakeIrq(interpreter);
    ChargeCycles(interpreter, cycle_accurate, 7);
    return 7;
  }
  if (interpreter->trace) {
    W65C02DisassemblePc(interpreter);
  }
  if (interpreter->stop_at_next_instruction) {
    DebuggerLoop(interpreter);
  }
  if (interpreter->bbc != NULL) {
    BbcMachineBeginInstruction(interpreter->bbc, interpreter->pc);
  }
  uint8_t opcode = interpreter->memory[interpreter->pc];
  // Columns x3, x7, xB and xF are one-byte, one-cycle NOPs on the G65SC12.
  // The other unused opcodes are multi-byte NOPs. Rockwell BBR/RMB and the
  // davecc $EF syscall are not part of this CPU.
  if (interpreter->sc12 && !(interpreter->debug && opcode == 0xff)) {
    int nop_bytes = 0;
    int nop_cycles = 0;
    switch (opcode & 0x0f) {
      case 0x03:
      case 0x07:
      case 0x0b:
      case 0x0f:
        nop_bytes = 1;
        nop_cycles = 1;
        break;
      default:
        break;
    }
    if (nop_cycles == 0) {
      switch (opcode) {
        case 0x02:
        case 0x22:
        case 0x42:
        case 0x44:
        case 0x62:
        case 0x82:
        case 0xc2:
        case 0xe2:
          nop_bytes = 2;
          nop_cycles = 2;
          break;
        case 0x54:
        case 0xd4:
        case 0xf4:
          nop_bytes = 2;
          nop_cycles = 4;
          break;
        case 0xdc:
        case 0xfc:
          nop_bytes = 3;
          nop_cycles = 4;
          break;
        case 0x5c:
          nop_bytes = 3;
          nop_cycles = 8;
          break;
        default:
          break;
      }
    }
    if (nop_cycles != 0) {
      interpreter->pc = (uint16_t)(interpreter->pc + nop_bytes);
      ChargeCycles(interpreter, cycle_accurate, nop_cycles);
      return nop_cycles;
    }
  }
  if (interpreter->nmos) {
    int nmos_cycles = NmosExecute(interpreter, opcode);
    if (nmos_cycles > 0) {
      cycles = nmos_cycles + interpreter->extra_cycles;
      if (cycles < 1) {
        cycles = 1;
      }
      ChargeCycles(interpreter, cycle_accurate, cycles);
      return cycles;
    }
  }
  Instruction* inst = &instructions[opcode];

  (*inst->func)(interpreter);
  cycles = inst->cycles + interpreter->extra_cycles;
  if (cycles < 1) {
    cycles = 1;
  }
  ChargeCycles(interpreter, cycle_accurate, cycles);
  return cycles;
}

static void InstallBbcDemo(uint8_t* ram) {
  static const uint8_t kDemo[] = {
      0x78, 0xA2, 0xFF, 0x9A, 0xD8, 0xA9, 0x4B, 0x8D, 0x20, 0xFE, 0xA2, 0x00, 0xBD, 0x74, 0xC0,
      0x8E, 0x00, 0xFE, 0x8D, 0x01, 0xFE, 0xE8, 0xE0, 0x0E, 0xD0, 0xF2, 0xA9, 0x00, 0x85, 0x70,
      0xA9, 0x7C, 0x85, 0x71, 0xA2, 0x04, 0xA9, 0x20, 0xA0, 0x00, 0x91, 0x70, 0xC8, 0xD0, 0xFB,
      0xE6, 0x71, 0xCA, 0xD0, 0xF4, 0xA2, 0x00, 0xBD, 0x82, 0xC0, 0xF0, 0x06, 0x9D, 0x50, 0x7C,
      0xE8, 0xD0, 0xF5, 0xA9, 0xFF, 0x8D, 0x43, 0xFE, 0xA9, 0x0F, 0x8D, 0x42, 0xFE, 0xA2, 0x00,
      0xBD, 0x8D, 0xC0, 0x8D, 0x41, 0xFE, 0xA9, 0x08, 0x8D, 0x40, 0xFE, 0xA9, 0x00, 0x8D, 0x40,
      0xFE, 0xE8, 0xE0, 0x06, 0xD0, 0xEB, 0x58, 0xAD, 0x4D, 0xFE, 0x29, 0x02, 0xF0, 0xF9, 0xA9,
      0x02, 0x8D, 0x4D, 0xFE, 0xEE, 0x28, 0x7C, 0x4C, 0x61, 0xC0, 0x40, 0x3F, 0x28, 0x33, 0x24,
      0x1E, 0x02, 0x19, 0x1B, 0x93, 0x12, 0x72, 0x13, 0x7C, 0x00, 0x03, 0x42, 0x42, 0x43, 0x20,
      0x4D, 0x69, 0x63, 0x72, 0x6F, 0x00, 0x8E, 0x1D, 0x90, 0xBF, 0xDF, 0xFF};
  memcpy(ram + 0xc000, kDemo, sizeof(kDemo));
  ram[0xfffa] = 0x73;
  ram[0xfffb] = 0xc0;
  ram[0xfffc] = 0x00;
  ram[0xfffd] = 0xc0;
  ram[0xfffe] = 0x73;
  ram[0xffff] = 0xc0;
}

bool W65C02InterpreterPrepareBbc(W65C02Interpreter* interpreter) {
  if (interpreter->bbc == NULL) {
    return false;
  }
  if (interpreter->memory == NULL) {
    interpreter->memory = calloc(65536, 1);
    if (interpreter->memory == NULL) {
      return false;
    }
  }
  interpreter->s = 0xff;
  interpreter->zero_page = interpreter->memory;
  interpreter->stack = interpreter->memory + 0x100;
  BbcMachineSetRam(interpreter->bbc, interpreter->memory);
  if (interpreter->bbc_os_path != NULL) {
    if (!BbcMachineLoadOs(interpreter->bbc, interpreter->bbc_os_path)) {
      return false;
    }
  } else {
    InstallBbcDemo(interpreter->memory);
    BbcMachineCaptureMos(interpreter->bbc);
  }
  BbcMachineMapSideways(interpreter->bbc);
  if (interpreter->bbc_initial_mode >= 0) {
    BbcMachineSelectMode(interpreter->bbc, interpreter->bbc_initial_mode);
  }
  interpreter->a = 0;
  interpreter->x = 0;
  interpreter->y = 0;
  interpreter->flags.value = 0;
  interpreter->flags.bits.i = 1;
  interpreter->flags.bits.x = 1;
  if (BbcMachineIsMaster(interpreter->bbc)) {
    interpreter->bbc_65c02 = true;
    interpreter->sc12 = true;
    interpreter->flags.bits.d = 0;
  }
  interpreter->nmos = !interpreter->bbc_65c02;
  interpreter->turbo = interpreter->bbc_os_path != NULL;
  interpreter->in_irq = 0;
  interpreter->jammed = false;
  interpreter->pc = (uint16_t)(interpreter->memory[0xfffc] |
                               (interpreter->memory[0xfffd] << 8));
  interpreter->running = true;
  interpreter->exit_code = 0;
  return true;
}

int W65C02InterpreterStep(W65C02Interpreter* interpreter) {
  if (!interpreter->running) {
    return 0;
  }
  return StepOneInstruction(interpreter, true);
}

void W65C02InterpreterResetCpu(W65C02Interpreter* interpreter) {
  if (interpreter->memory == NULL) {
    return;
  }
  interpreter->s = 0xff;
  interpreter->flags.bits.i = 1;
  interpreter->pc = (uint16_t)(interpreter->memory[0xfffc] |
                               (interpreter->memory[0xfffd] << 8));
  interpreter->running = true;
  interpreter->jammed = false;
  interpreter->in_irq = 0;
  interpreter->turbo_owed = 0;
  interpreter->pace_cycles = 0;
  interpreter->pace_stores = 0;
  interpreter->pace_sheila = 0;
  interpreter->pace_watch = 0;
  interpreter->pace_busy = 0;
  interpreter->turbo = interpreter->bbc_os_path != NULL;
  if (interpreter->sc12) {
    interpreter->flags.bits.d = 0;
  }
  if (interpreter->bbc != NULL) {
    BbcMachineBreak(interpreter->bbc);
    BbcMachineResetFdc(interpreter->bbc);
  }
}

void W65C02InterpreterBbcUse65C02(W65C02Interpreter* interpreter, bool enable) {
  interpreter->bbc_65c02 = enable;
  if (enable) {
    interpreter->nmos = false;
    interpreter->jammed = false;
  }
}

void W65C02Reset(W65C02Interpreter* interpreter) {
  // Write entry address into zero page 0,1
  interpreter->memory[0] = interpreter->entry_address & 0xff;
  interpreter->memory[1] = (interpreter->entry_address >> 8) & 0xff;

  // Start execution at the reset handler.
  interpreter->pc = interpreter->memory[0xfffc] | interpreter->memory[0xfffd] << 8;
  
  interpreter->stop_at_next_instruction = false;
}

#define RESET_HANDLER 0xff00
#define IRQ_HANDLER 0xff80

#define VECTOR_RAM 0xfd00

static bool W65C02CopyGuestArgv(W65C02Interpreter* interpreter, int argc,
                                char** argv, int first_arg,
                                int* guest_argc_out) {
  if (argv == NULL || first_arg <= 0 || first_arg > argc) {
    return false;
  }

  int guest_argc = argc - first_arg + 1;
  size_t pointer_bytes = (size_t)(guest_argc + 1) * 2;
  size_t argv_capacity = W65C02_GUEST_ARGV_END - W65C02_GUEST_ARGV_START;
  if (pointer_bytes > argv_capacity) {
    return false;
  }
  size_t string_bytes = 0;
  for (int i = first_arg - 1; i < argc; ++i) {
    size_t len = strlen(argv[i]) + 1;
    if (len > argv_capacity - pointer_bytes - string_bytes) {
      return false;
    }
    string_bytes += len;
  }

  uint16_t string_address =
      (uint16_t)(W65C02_GUEST_ARGV_START + pointer_bytes);
  for (int i = 0; i < guest_argc; ++i) {
    int host_index = first_arg - 1 + i;
    size_t len = strlen(argv[host_index]) + 1;
    uint16_t pointer_address =
        (uint16_t)(W65C02_GUEST_ARGV_START + i * 2);
    interpreter->memory[pointer_address] = string_address & 0xff;
    interpreter->memory[pointer_address + 1] = string_address >> 8;
    memcpy(interpreter->memory + string_address, argv[host_index], len);
    string_address = (uint16_t)(string_address + len);
  }
  uint16_t terminator =
      (uint16_t)(W65C02_GUEST_ARGV_START + guest_argc * 2);
  interpreter->memory[terminator] = 0;
  interpreter->memory[terminator + 1] = 0;
  *guest_argc_out = guest_argc;
  return true;
}

int W65C02InterpreterRun(W65C02Interpreter* interpreter, Loader* loader,
                         uint64_t entry_address, int argc, char** argv,
                         int first_arg) {
  interpreter->memory = calloc(65536, 1);    // 64K of memory.
  interpreter->s = 0xff;
  interpreter->zero_page = (uint8_t*)interpreter->memory;
  interpreter->stack = (uint8_t*)interpreter->memory + interpreter->s + 0x100;
  interpreter->loader = loader;
  if (interpreter->bbc != NULL) {
    BbcMachineSetRam(interpreter->bbc, interpreter->memory);
  }
  
  // Load the support ROM unless a BBC OS image will occupy &C000.
  if (interpreter->rom_filename.length > 0) {
    Loader rom_loader;
    bool ok = LoaderInitFromFile(&rom_loader,
                                 &interpreter->rom_filename,
                                 loader->flags, loader->arch, loader->arch_data, NULL);
    if (!ok) {
      fprintf(stderr, "Unable to load ROM from file '%s'\n", interpreter->rom_filename.value);
      exit(1);
    }
    for (size_t i = 1; i < rom_loader.regions.length; i++) {
      Region* region = rom_loader.regions.value.p[i];
      for (size_t section_index = 0; section_index < region->sections.length; section_index++) {
        ELFReaderSection* section = region->sections.value.p[section_index];
        void* section_addr = (char*)region->address + section->header->offset - region->offset;
        memcpy(interpreter->memory+section->header->addr, section_addr, section->header->size);
      }
    }
  }
 
  
  // Find address of __enter because the entry sequence contains
  // a register save mask (3 bytes after JSR __enter).
  interpreter->enter_func = LoaderLookupSymbol(loader, "__enter");
  interpreter->enter_leaf_func = LoaderLookupSymbol(loader, "__enter_leaf");

  // Copy the memory mapped in from the file into the interpreter's
  // memory.
  int memtop = 0;
  for (size_t i = 1; i < loader->regions.length; i++) {
    Region* region = loader->regions.value.p[i];
    for (size_t section_index = 0; section_index < region->sections.length; section_index++) {
      ELFReaderSection* section = region->sections.value.p[section_index];
      int top = (int)section->header->addr + (int)section->header->size;
      if (section->header->type == SHT(nobits)) {
        if (top > memtop) {
          memtop = top;
        }
        continue;
      }
      void* section_addr = (char*)region->address + section->header->offset - region->offset;
      memcpy(interpreter->memory+section->header->addr, section_addr, section->header->size);
      if (top > memtop) {
        memtop = top;
      }
    }
  }
  if (interpreter->bbc != NULL) {
    if (interpreter->bbc_os_path != NULL &&
        !BbcMachineLoadOs(interpreter->bbc, interpreter->bbc_os_path)) {
      exit(1);
    }
    BbcMachineMapSideways(interpreter->bbc);
    if (interpreter->bbc_initial_mode >= 0) {
      BbcMachineSelectMode(interpreter->bbc, interpreter->bbc_initial_mode);
    }
    // An OS image is a Model B, so the CPU is the NMOS 6502. -bbc-65c02
    // keeps the CMOS core. A Master always runs the 65C02. A davecc program
    // with no OS image stays a 65C02.
    if (interpreter->bbc_os_path != NULL) {
      if (BbcMachineIsMaster(interpreter->bbc)) {
        interpreter->bbc_65c02 = true;
      }
      interpreter->nmos = !interpreter->bbc_65c02;
      interpreter->jammed = false;
    }
  }
  int num_args = 0;
  // Need some space for the stack.
  if (memtop > 0xbe00) {
    printf("Program is too big\n");
    exit(1);
  }
  interpreter->entry_address = (int)entry_address;
  
  W65C02SetupGuestStartupStack(interpreter);
  if (!W65C02GuestRunInitArrays(loader, interpreter)) {
    fprintf(stderr, "Error running guest init arrays\n");
    exit(1);
  }

  // Page 5 through the start of the linked program is reserved for argv.
  // Populate it after loading sections and running constructors so neither
  // can overwrite it. Include argv[0] and the required null sentinel.
  if (!W65C02CopyGuestArgv(interpreter, argc, argv, first_arg, &num_args)) {
    fprintf(stderr, "Program arguments exceed 6502 guest argv space\n");
    return 1;
  }

  // Guest constructors use the normal zero-page calling convention and may
  // clobber i0. Restore argc only after they finish so _start can push the
  // correct value for main.
  interpreter->memory[REG_I0] = num_args & 0xff;
  interpreter->memory[REG_I0 + 1] = (num_args >> 8) & 0xff;

  // Write entry address into zero page 0,1
  interpreter->memory[0] = entry_address & 0xff;
  interpreter->memory[1] = (entry_address >> 8) & 0xff;

  // Start execution at the reset handler.
  interpreter->pc = interpreter->memory[0xfffc] | interpreter->memory[0xfffd] << 8;

  if (interpreter->debug) {
    interpreter->stop_at_next_instruction = true;
  }

  interpreter->running = true;
  interpreter->exit_code = 0;

  // Run processor by fetching instruction at PC and jumping to the handler
  // function for that opcode.  Each handler function will set the PC to
  // the next instruction address.
  while (interpreter->running) {
    StepOneInstruction(interpreter, true);
  }
  if (!W65C02GuestRunFiniArrays(loader, interpreter)) {
    return 1;
  }
  return interpreter->exit_code;
}

void W65C02InterpreterDisassemble(W65C02Interpreter* interpreter, Loader* loader) {
  for (size_t i = 1; i < loader->regions.length; i++) {
    Region* region = loader->regions.value.p[i];
    for (size_t section_index = 0; section_index < region->sections.length; section_index++) {
      ELFReaderSection* section = region->sections.value.p[section_index];
      if ((section->header->flags & SHF(execinstr)) != 0) {
        void* addr = (void*)((char*)region->address + section->header->offset);
        void* end_addr = addr + section->header->size;
        uint16_t pc = section->header->addr;
        SymbolScope* symbol = NULL;
        while (addr <= end_addr) {
          symbol =
              LoaderFindSymbolAndCacheResult(loader, pc);
          void* new_addr = Disassemble6502Instruction(loader, symbol, pc, addr, stdout);
          pc += new_addr - addr;
          addr = new_addr;
        }
      }
    }
  }
  
}

// Extract the contents of the EXE file into a binary file containing
// only the sections with data.  This will be .text and .data sections
// containing code and static data.  This can be burned into a ROM.
void W65C02InterpreterExtract(W65C02Interpreter* interpreter, Loader* loader, FILE* fp) {
  char* memory = calloc(65536, 1);
  char* start_memory = memory + 65536;
  char* end_memory = memory;
  
  // Copy all section contents to the temporary memory at the correct address.
  for (size_t i = 1; i < loader->regions.length; i++) {
    Region* region = loader->regions.value.p[i];
    for (size_t section_index = 0; section_index < region->sections.length; section_index++) {
      ELFReaderSection* section = region->sections.value.p[section_index];
      if (section->header->size == 0) {
        continue;
      }
      void* section_addr = (char*)region->address + section->header->offset;
      memcpy(memory+section->header->addr, section_addr, section->header->size);
      
      // Calculate limits of data.
      char* start_addr = memory+section->header->addr;
      if (start_addr < start_memory) {
        start_memory = start_addr;
      }
      char* end_addr = memory+section->header->addr + section->header->size;
      if (end_addr > end_memory) {
        end_memory = end_addr;
      }
    }
  }
  // Write the memory to the file.
  fwrite(start_memory, end_memory - start_memory, 1, fp);
  free(memory);
}

void W65C02InterpreterDestruct(W65C02Interpreter* interpreter) {
  size_t i;
  if (interpreter->bbc != NULL) {
    if (interpreter->bbc_screen_path != NULL && interpreter->bbc_screen_path[0] != '\0') {
      if (!BbcMachineWritePpm(interpreter->bbc, interpreter->bbc_screen_path)) {
        fprintf(stderr, "Unable to write BBC screen to '%s'\n",
                interpreter->bbc_screen_path);
      }
    }
    BbcMachineDestroy(interpreter->bbc);
    interpreter->bbc = NULL;
  }
  for (i = 0; i < interpreter->devices.length; i++) {
    free(interpreter->devices.value.p[i]);
  }
  VectorDestruct(&interpreter->devices);
  free(interpreter->memory);
  VectorDestruct(&interpreter->breakpoints);
  VectorDestruct(&interpreter->watchpoints);
}

// Stack is empty/descending.  SP starts at 0xff and always points to the next
// location to use.
// Push PC plus an increment, onto stack.  Pushed high byte first.
#define PUSH_PC(inc) {\
  *(interpreter->memory + 0x100 + interpreter->s--) = (interpreter->pc + inc) >> 8;\
  *(interpreter->memory + 0x100 + interpreter->s--) = (interpreter->pc + inc)& 0xff; \
  }

// Pop PC from the stack
#define POP_PC(inc) {\
  uint16_t v = *(interpreter->memory + 0x100 + ++interpreter->s); \
  v |= *(interpreter->memory + 0x100 + ++interpreter->s) << 8; \
  interpreter->pc = v + inc; \
}

// Puah an 8-bit value onto the stack.
#define PUSH(value) *(uint8_t*)(interpreter->memory + 0x100 + interpreter->s--) = value;

// Pop stack into register.
#define POP(reg) interpreter->reg = interpreter->memory[0x100 + ++interpreter->s]

// Jump to an address.
#define JMP(addr) interpreter->pc = addr

// 8-git operand in instruction.
#define OP8() interpreter->memory[interpreter->pc+1]

// 16-bit operand in instruction.
#define OP16() *(uint16_t*)(interpreter->memory + interpreter->pc+1)

// LDA and STA already consult memory-mapped devices. ALU and BIT reads
// must too: the MOS masks the VIA interrupt flags with AND and scans the
// keyboard with BIT.
static void NoteSlowBus(W65C02Interpreter* interpreter, uint16_t addr) {
  // FRED, JIM, and SHEILA are on the 1 MHz bus. The 2 MHz CPU is stretched
  // by one cycle to meet that clock.
  if (interpreter->bbc != NULL && addr >= 0xfc00 && addr <= 0xfeff) {
    interpreter->extra_cycles++;
  }
}

static uint8_t W65C02ReadByte(W65C02Interpreter* interpreter, uint16_t addr) {
  uint8_t* p = interpreter->memory + addr;
  if (W65C02MemIsIo(interpreter, p)) {
    W65C02CatchUp(interpreter);
    NoteSlowBus(interpreter, addr);
    W65C02NoteRead(interpreter, addr);
    Device* dev = PollForDevice(interpreter, p);
    if (dev != NULL) {
      return (uint8_t)dev->read(dev, addr);
    }
  }
  return *p;
}

static void W65C02WriteByte(W65C02Interpreter* interpreter, uint16_t addr, uint8_t value) {
  uint8_t* p = interpreter->memory + addr;
  if (W65C02MemIsIo(interpreter, p)) {
    W65C02CatchUp(interpreter);
    NoteSlowBus(interpreter, addr);
    Device* dev = PollForDevice(interpreter, p);
    if (dev != NULL) {
      dev->write(dev, addr, value);
      W65C02NoteStore(interpreter, addr);
      return;
    }
  }
  if (!BbcMachineSidewaysRom(interpreter->bbc, addr)) {
    *p = value;
    W65C02NoteStore(interpreter, addr);
  }
}

// TSB and TRB set Z from A & M and leave N and V alone. The write has to
// reach SHEILA: MOS 3.50 resets the Master with TRB $FE34.
static void TestAndChangeBits(W65C02Interpreter* interpreter, uint16_t addr, bool set_bits) {
  uint8_t mem = W65C02ReadByte(interpreter, addr);
  interpreter->flags.bits.z = (interpreter->a & mem) == 0;
  W65C02WriteByte(interpreter, addr,
                  set_bits ? (uint8_t)(mem | interpreter->a)
                           : (uint8_t)(mem & (uint8_t)~interpreter->a));
}

#define SET_ZERO(_v) interpreter->flags.bits.z = (_v) == 0
#define SET_SIGN(_v) interpreter->flags.bits.s = ((_v) & 0x80) != 0
#define SET_OVERFLOW(_v) interpreter->flags.bits.v = (_v) != 0

// Set the z and s flags.
#define SET_FLAGS(value) {\
  uint8_t _v = value;\
  SET_ZERO(_v);\
  SET_SIGN(_v);\
}


#define SET_CMP_FLAGS(reg, src, result) {\
  uint8_t _v = result;\
  SET_ZERO(_v);\
  int overflow = (((~(interpreter->reg ^ src)) & (interpreter->reg ^ result)) & 0x80) != 0; \
  int negative = (_v & 0x80) != 0; \
  interpreter->flags.bits.s = (negative ^ overflow) != 0; \
}

// Set overflow flag based on accumulator value, the src value being
// used and the result of a calculation.
#define SET_OVERFLOW_A(src, result) \
SET_OVERFLOW(((~(interpreter->a ^ src)) & (interpreter->a ^ result)) & 0x80);

// Set the carry flag from a bit in a value.
#define SET_CARRY_BIT(value, bit) interpreter->flags.bits.c = (value & (1 << bit)) != 0

// Set the carry to a value.
#define SET_CARRY_VALUE(value) interpreter->flags.bits.c = (value) != 0

// Set the borrow flag (inverse of carry).
#define SET_BORROW_BIT(value, bit) interpreter->flags.bits.c = (value & (1 << bit)) == 0

#define SET_BORROW_VALUE(value) interpreter->flags.bits.c = (value) == 0

// 8-bit zero page value from an address.
#define ZP(addr) interpreter->zero_page[addr]

// 16-bit zero page address from an address.
#define ZP_ADDR(addr) &interpreter->zero_page[addr]

// 8-bit value from memory at address.
#define MEM(addr) interpreter->memory[addr]

// 16-bit address from memory at address.
#define MEM_ADDR(addr) (uint8_t*)(interpreter->memory + addr)

// Increment the PC by the given number of bytes.
#define INC_PC(inc) interpreter->pc += inc

// A 16-bit address formed by adding an absolute address to an index register.
#define INDEXED(addr, reg) (interpreter->memory + addr + interpreter->reg)

// An 8-bit value read from memory at the given address
#define INDIRECT(addr) *(uint8_t*)(interpreter->memory + addr)

// 16-bit value at address.  Used to set PC.
#define INDIRECT2(addr) *(uint16_t*)(interpreter->memory + addr)

// A 16-bit address read from memory at the given address
#define INDIRECT_ADDR(addr) (uint8_t*)(interpreter->memory + *(uint16_t*)(interpreter->memory + addr))

// An address  read from the address formed by adding an index register to a
// zero page address, then taking the contents of that 16-bit address.
#define INDEXED_INDIRECT(addr, reg) (interpreter->memory + ((*(uint16_t*)(interpreter->memory + addr + interpreter->reg))))

// An address read from the address formed by taking the contents of 2 bytes
// in zero page and then adding an index register.
#define INDIRECT_INDEXED(addr, reg) (interpreter->memory + (*(uint16_t*)(interpreter->memory + addr) + interpreter->reg))

// 16-bit address formed from the address formed by adding an index register to a
// zero page address, then taking the contents of that 16-bit address.
#define INDEXED_INDIRECT_ADDR(addr, reg) (interpreter->memory + (*(uint16_t*)(interpreter->memory + addr + interpreter->reg)))

// A 16-bit address formed from the address formed by taking the contents of 2 bytes
// in zero page and then adding an index register.
#define INDIRECT_INDEXED_ADDR(addr, reg) (interpreter->memory + *((uint16_t*)(interpreter->memory + addr) + interpreter->reg))

// Load an 8-bit immediate into a register.
#define SET_IMMED(reg, value) interpreter->reg = value

// Load a register with a value.
#define LOAD(reg, addr) { \
  if (!W65C02MemIsIo(interpreter, addr)) {\
    if (IsWatchedAddress(interpreter, kWatchRead, addr, interpreter->reg)) { \
        DebuggerLoop(interpreter);\
    } \
    SET_FLAGS(interpreter->reg = *(addr));\
  } else {\
    W65C02CatchUp(interpreter); \
    NoteSlowBus(interpreter, (uint16_t)(addr - interpreter->memory)); \
    W65C02NoteRead(interpreter, (uint16_t)(addr - interpreter->memory)); \
    Device* dev = PollForDevice(interpreter, addr); \
    if (dev != NULL) { \
      SET_FLAGS(interpreter->reg = dev->read(dev, addr - interpreter->memory)); \
    } else { \
      SET_FLAGS(interpreter->reg = *(addr));\
    }\
  }\
}

#define LOAD_ZP(reg, addr)     if (IsWatchedAddress(interpreter, kWatchRead, addr, interpreter->reg)) { \
    DebuggerLoop(interpreter);\
} \
SET_FLAGS(interpreter->reg = *(addr));

// Store a register into an address.
#define STORE(reg, addr) { \
  if (!W65C02MemIsIo(interpreter, addr)) {\
    if (IsWatchedAddress(interpreter, kWatchWrite, addr, interpreter->reg)) { \
    DebuggerLoop(interpreter);\
  } \
    if (!BbcMachineSidewaysRom(interpreter->bbc, (uint16_t)((addr) - interpreter->memory))) { \
      *(addr) = interpreter->reg;\
      W65C02NoteStore(interpreter, (uint16_t)((addr) - interpreter->memory)); \
    } \
  } else {\
    W65C02CatchUp(interpreter); \
    NoteSlowBus(interpreter, (uint16_t)(addr - interpreter->memory)); \
    Device* dev = PollForDevice(interpreter,addr);\
    if (dev != NULL) {\
      dev->write(dev, addr - interpreter->memory, interpreter->reg); \
    } else { \
      *(addr) = interpreter->reg;\
    }\
    W65C02NoteStore(interpreter, (uint16_t)((addr) - interpreter->memory)); \
  }\
}

#define STORE_ZP(reg, addr)   if (IsWatchedAddress(interpreter, kWatchWrite, addr, interpreter->reg)) { \
  DebuggerLoop(interpreter);\
} \
*(addr) = interpreter->reg; \
W65C02NoteStore(interpreter, (uint16_t)((addr) - interpreter->memory));

#define STORE_ZERO(addr) { \
  if (!W65C02MemIsIo(interpreter, addr)) {\
    if (IsWatchedAddress(interpreter, kWatchWrite, addr, 0)) { \
      DebuggerLoop(interpreter);\
    } \
    if (!BbcMachineSidewaysRom(interpreter->bbc, (uint16_t)((addr) - interpreter->memory))) { \
      *(addr) = 0;\
      W65C02NoteStore(interpreter, (uint16_t)((addr) - interpreter->memory)); \
    } \
  } else {\
    W65C02CatchUp(interpreter); \
    NoteSlowBus(interpreter, (uint16_t)((addr) - interpreter->memory)); \
    Device* dev = PollForDevice(interpreter, addr);\
    if (dev != NULL) {\
      dev->write(dev, (addr) - interpreter->memory, 0); \
    } else {\
      *(addr) = 0;\
    }\
    W65C02NoteStore(interpreter, (uint16_t)((addr) - interpreter->memory)); \
  }\
}

#define ADD_WITH_CARRY(value) CpuAdd(interpreter, (uint8_t)(value))

#define SUB_WITH_BORROW(value) CpuSub(interpreter, (uint8_t)(value))

#define ALU_OP_INDEXED_INDIRECT_ZP(index, op) { \
  uint16_t addr = OP8();\
  uint16_t value = *INDEXED_INDIRECT(addr, index);\
  SET_FLAGS(interpreter->a op##= value); \
  INC_PC(2); \
}

#define PAGE_PENALTY(base, index)                                              \
  if ((((uint16_t)(base) & 0xff) + (uint16_t)(interpreter->index)) > 0xff)     \
    interpreter->extra_cycles++

#define ALU_OP_INDIRECT_INDEXED_ZP(index, op) { \
  uint16_t addr = OP8();\
  uint16_t ptr = *(uint16_t*)(interpreter->memory + addr);\
  PAGE_PENALTY(ptr, index);\
  uint16_t value = *INDIRECT_INDEXED(addr, index);\
  SET_FLAGS(interpreter->a op##= value); \
  INC_PC(2); \
}

#define ALU_IMMED(op) {\
  SET_FLAGS(interpreter->a op##= OP8());\
  INC_PC(2);\
}

#define ALU_OP_ZP(op) {\
  SET_FLAGS(interpreter->a op##= interpreter->zero_page[OP8()]);\
  INC_PC(2);\
}

#define ALU_OP_ABS(op) {\
  uint16_t addr = OP16();\
  SET_FLAGS(interpreter->a op##= W65C02ReadByte(interpreter, addr));\
  INC_PC(3);\
}

#define ALU_OP_ZP_INDIRECT(op) {\
  uint16_t addr = OP8();\
  uint8_t value = INDIRECT(addr);\
  SET_FLAGS(interpreter->a op##= value);\
  INC_PC(2);\
}

#define ALU_OP_ZP_INDEXED(index, op) {\
  uint16_t addr = OP8();\
  uint8_t value = INDIRECT(addr + interpreter->index);\
  SET_FLAGS(interpreter->a op##= value);\
  INC_PC(2);\
}

#define ALU_OP_ABS_INDEXED(index, op) {\
  uint16_t base = OP16();\
  PAGE_PENALTY(base, index);\
  uint16_t addr = base + interpreter->index;\
  uint8_t value = W65C02ReadByte(interpreter, addr);\
  SET_FLAGS(interpreter->a op##= value);\
  INC_PC(3);\
}

#define BRANCH(flag, value) {\
  if (interpreter->flags.bits.flag == value) {\
    int8_t offset = OP8();\
    uint16_t base = (uint16_t)(interpreter->pc + 2);\
    uint16_t dest = (uint16_t)(base + offset);\
    interpreter->extra_cycles++;\
    if ((base & 0xff00) != (dest & 0xff00)) interpreter->extra_cycles++;\
    interpreter->pc = dest;\
    return;\
  }\
  INC_PC(2);\
}

#define UNIMPLEMENTED(inst)                                                    \
  {                                                                            \
    fprintf(stderr, "Unimplemented instruction %s at 0x%04x\n", #inst,         \
            interpreter->pc);                                                  \
    abort();                                                                   \
  }

// Instruction definitions.


// *************************************************************************
// Miscellaneous
// *************************************************************************

W65C02_instruction_def(brk) {
  PUSH_PC(2);
  PUSH(interpreter->flags.value | (3 << 4));    // Push P with bits 4 and 5 fiag set.
  interpreter->flags.bits.i = 1;
  Sc12LeaveDecimal(interpreter);
  interpreter->in_irq++;
  uint16_t vector = interpreter->memory[0xfffe] + (interpreter->memory[0xffff] << 8);
  JMP(vector);
}

W65C02_instruction_def(nop) {
  INC_PC(1);
}

// *************************************************************************
// ORA
// *************************************************************************
W65C02_instruction_def(ora_zp_x_indirect) {
  ALU_OP_INDEXED_INDIRECT_ZP(x, |);
}

W65C02_instruction_def(ora_zp) {
  ALU_OP_ZP(|);
}

W65C02_instruction_def(ora_immed) {
  ALU_IMMED(|);
}


W65C02_instruction_def(ora_abs) {
  ALU_OP_ABS(|);
}

W65C02_instruction_def(ora_abl) {
  UNIMPLEMENTED(ora_abl);
}

W65C02_instruction_def(ora_zp_indirect_y) {
  ALU_OP_INDIRECT_INDEXED_ZP(y, |);
}

W65C02_instruction_def(ora_zp_indirect) {
  ALU_OP_ZP_INDIRECT(|);
}

W65C02_instruction_def(ora_zp_s_indirect_y) {
  UNIMPLEMENTED(ora_zp_s_indirect_y);
}

W65C02_instruction_def(ora_zp_x) {
  ALU_OP_ZP_INDEXED(x, |);
}

W65C02_instruction_def(ora_abs_x) {
  ALU_OP_ABS_INDEXED(x, |);
}

W65C02_instruction_def(ora_zp_y) {
  ALU_OP_ZP_INDEXED(y, |);
}

W65C02_instruction_def(ora_abs_y) {
  ALU_OP_ABS_INDEXED(y, |);
}


// *************************************************************************
// AND
// *************************************************************************

W65C02_instruction_def(and_zp_x_indirect) {
  ALU_OP_INDEXED_INDIRECT_ZP(x, &);
}

W65C02_instruction_def(and_zp_s) {
  UNIMPLEMENTED(and_zp_s);
}

W65C02_instruction_def(and_zp) {
  ALU_OP_ZP(&);
}

W65C02_instruction_def(and_immed) {
  ALU_IMMED(&);
}

W65C02_instruction_def(and_abs) {
  ALU_OP_ABS(&);
}

W65C02_instruction_def(and_al) {
  UNIMPLEMENTED(and_al);
}

W65C02_instruction_def(and_zp_indirect_y) {
  ALU_OP_INDIRECT_INDEXED_ZP(y, &);
}
W65C02_instruction_def(and_zp_indirect) {
  ALU_OP_ZP_INDIRECT(&);
  
}
W65C02_instruction_def(and_zp_s_indirect_y) {
  ALU_OP_INDIRECT_INDEXED_ZP(y, &);
}

W65C02_instruction_def(and_zp_x) {
  ALU_OP_ZP_INDEXED(x, &);
}

W65C02_instruction_def(and_zp_y) {
  ALU_OP_ZP_INDEXED(y, &);
}

W65C02_instruction_def(and_abs_y) {
  ALU_OP_ABS_INDEXED(y, &);
}

W65C02_instruction_def(and_abs_x) {
  ALU_OP_ABS_INDEXED(x, &);
}

// *************************************************************************
// ASL
// *************************************************************************

W65C02_instruction_def(asl_zp) {
  uint16_t addr = OP8();
  SET_CARRY_BIT(ZP(addr), 7);
  SET_FLAGS(ZP(addr) <<= 1);
  INC_PC(2);
}

W65C02_instruction_def(asl_accum) {
  SET_CARRY_BIT(interpreter->a, 7);
  SET_FLAGS(interpreter->a <<= 1);
  INC_PC(1);
}

W65C02_instruction_def(asl_abs_x) {
  uint16_t addr = OP16() + interpreter->x;
  SET_CARRY_BIT(MEM(addr), 7);
  SET_FLAGS(MEM(addr) <<= 1);
  INC_PC(3);
}

W65C02_instruction_def(asl_zp_x) {
  uint16_t addr = OP8() + interpreter->x;
  SET_CARRY_BIT(MEM(addr), 7);
  SET_FLAGS(MEM(addr) <<= 1);
  INC_PC(2);
}

W65C02_instruction_def(asl_abs) {
  uint16_t addr = OP16();
  SET_CARRY_BIT(MEM(addr), 7);
  SET_FLAGS(MEM(addr) <<= 1);
  INC_PC(3);
}


// *************************************************************************
// LSR
// *************************************************************************

W65C02_instruction_def(lsr_accum) {
  SET_CARRY_BIT(interpreter->a, 0);
  SET_FLAGS(interpreter->a >>= 1);
  INC_PC(1);
}

W65C02_instruction_def(lsr_zp) {
  uint16_t addr = OP8();
  SET_CARRY_BIT(ZP(addr), 0);
  SET_FLAGS(ZP(addr) >>= 1);
  INC_PC(2);

}

W65C02_instruction_def(lsr_abs) {
  uint16_t addr = OP16();
  SET_CARRY_BIT(MEM(addr), 0);
  SET_FLAGS(MEM(addr) >>= 1);
  INC_PC(3);
}

W65C02_instruction_def(lsr_zp_x) {
  uint16_t addr = OP8();
  uint8_t* p = INDEXED(addr,x);
  SET_CARRY_BIT(*p, 0);
  SET_FLAGS(*p >>= 1);
  INC_PC(2);
}

W65C02_instruction_def(lsr_abs_x) {
  uint16_t addr = OP16();
  uint8_t* p = INDEXED(addr, x);
  SET_CARRY_BIT(*p, 0);
  SET_FLAGS(*p >>= 1);
  INC_PC(3);
}

// *************************************************************************
// ROL
// *************************************************************************

W65C02_instruction_def(rol_zp) {
  uint16_t addr = OP8();
  int carry = interpreter->flags.bits.c;
  SET_CARRY_BIT(ZP(addr), 7);
  int v = ZP(addr) << 1 | carry;
  SET_FLAGS(ZP(addr) = v);
  INC_PC(2);
}

W65C02_instruction_def(rol_accum) {
  int carry = interpreter->flags.bits.c;
  SET_CARRY_BIT(interpreter->a, 7);
  int v = interpreter->a << 1 | carry;
  SET_FLAGS(interpreter->a = v);
  INC_PC(1);
}

W65C02_instruction_def(rol_abs) {
  uint16_t addr = OP16();
  int carry = interpreter->flags.bits.c;
  SET_CARRY_BIT(MEM(addr), 7);
  int v = MEM(addr) << 1 | carry;
  SET_FLAGS(MEM(addr) = v);
  INC_PC(3);
}

W65C02_instruction_def(rol_zp_x) {
  uint16_t addr = OP8();
  int carry = interpreter->flags.bits.c;
  uint8_t* p = INDEXED(addr,x);
  SET_CARRY_BIT(*p, 7);
  int v = *p << 1 | carry;
  SET_FLAGS(*p = v);
  INC_PC(2);
}

W65C02_instruction_def(rol_abs_x) {
  uint16_t addr = OP16();
  int carry = interpreter->flags.bits.c;
  uint8_t* p = INDEXED(addr,x);
  int v = *p << 1 | carry;
  SET_CARRY_BIT(*p, 7);
  SET_FLAGS(*p = v);
  INC_PC(3);
}

// *************************************************************************
// ROR
// *************************************************************************

W65C02_instruction_def(ror_zp) {
  uint16_t addr = OP8();
  int carry = interpreter->flags.bits.c;
  SET_CARRY_BIT(ZP(addr), 0);
  int v = ZP(addr) >> 1 | carry << 7;
  SET_FLAGS(ZP(addr) = v);
  INC_PC(2);
}

W65C02_instruction_def(ror_accum) {
  int carry = interpreter->flags.bits.c;
  SET_CARRY_BIT(interpreter->a, 0);
  int v = interpreter->a >> 1 | carry << 7;
  SET_FLAGS(interpreter->a = v);
  INC_PC(1);
}

W65C02_instruction_def(ror_abs) {
  uint16_t addr = OP16();
  int carry = interpreter->flags.bits.c;
  SET_CARRY_BIT(MEM(addr), 0);
  int v = MEM(addr) >> 1 | carry << 7;
  SET_FLAGS(MEM(addr) = v);
  INC_PC(3);
}

W65C02_instruction_def(ror_zp_x) {
  uint16_t addr = OP8();
  int carry = interpreter->flags.bits.c;
  uint8_t* p = INDEXED(addr, x);
  SET_CARRY_BIT(*p, 0);
  int v = *p >> 1 | carry << 7;
  SET_FLAGS(*p = v);
  INC_PC(2);
}

W65C02_instruction_def(ror_abs_x) {
  uint16_t addr = OP16();
  int carry = interpreter->flags.bits.c;
  uint8_t* p = INDEXED(addr, x);
  SET_CARRY_BIT(*p, 0);
  int v = *p >> 1 | carry << 7;
  SET_FLAGS(*p = v);
  INC_PC(3);
}


// *************************************************************************
// EOR
// *************************************************************************

W65C02_instruction_def(eor_zp_x_indirect) {
  ALU_OP_INDEXED_INDIRECT_ZP(x, ^);
}

W65C02_instruction_def(eor_zp) {
  ALU_OP_ZP(^);
}

W65C02_instruction_def(eor_zp_s) {
  UNIMPLEMENTED(eor_zp_s);
}

W65C02_instruction_def(eor_immed) {
  ALU_IMMED(^);
}

W65C02_instruction_def(eor_abs) {
  ALU_OP_ABS(^);
}

W65C02_instruction_def(eor_zp_indirect_y) {
  ALU_OP_INDIRECT_INDEXED_ZP(y, ^);
}
W65C02_instruction_def(eor_zp_indirect) {
  ALU_OP_ZP_INDIRECT(^);
}
W65C02_instruction_def(eor_zp_s_indirect_y) {
  UNIMPLEMENTED(eor_zp_s_indirect_y);
}


W65C02_instruction_def(eor_zp_x) {
  ALU_OP_ZP_INDEXED(x, ^);
}

W65C02_instruction_def(eor_abs_y) {
  ALU_OP_ABS_INDEXED(y, ^);
}

W65C02_instruction_def(eor_abs_x) {
  ALU_OP_ABS_INDEXED(x, ^);
}


// *************************************************************************
// LDA
// *************************************************************************

W65C02_instruction_def(lda_zp_x_indirect) {
  LOAD(a, INDEXED_INDIRECT(OP8(), x));
  INC_PC(2);
}

W65C02_instruction_def(lda_zp_s) {
  UNIMPLEMENTED(lda_zp_s);
}

W65C02_instruction_def(lda_zp) {
  LOAD_ZP(a, ZP_ADDR(OP8()));
  INC_PC(2);
}

W65C02_instruction_def(lda_zp_indirect) {
  uint16_t addr = OP8();
  LOAD(a, INDIRECT_ADDR(addr));
  INC_PC(2);
}

W65C02_instruction_def(lda_immed) {
  uint8_t v = OP8();
  SET_FLAGS(interpreter->a = v);
  INC_PC(2);
}

W65C02_instruction_def(lda_abs) {
  LOAD(a, MEM_ADDR(OP16()));
  INC_PC(3);
}


W65C02_instruction_def(lda_zp_indirect_y) {
  uint8_t zp = OP8();
  PAGE_PENALTY(*(uint16_t*)(interpreter->memory + zp), y);
  LOAD(a, INDIRECT_INDEXED(zp, y));
  INC_PC(2);
}

W65C02_instruction_def(lda_zp_s_indirect_y) {
  UNIMPLEMENTED(lda_zp_s_indirect_y);
}

W65C02_instruction_def(lda_zp_x) {
  LOAD_ZP(a, INDEXED(OP8(), x));
  INC_PC(2);
}

W65C02_instruction_def(lda_abs_y) {
  uint16_t base = OP16();
  PAGE_PENALTY(base, y);
  LOAD(a, INDEXED(base, y));
  INC_PC(3);
}

W65C02_instruction_def(lda_abs_x) {
  uint16_t base = OP16();
  PAGE_PENALTY(base, x);
  LOAD(a, INDEXED(base, x));
  INC_PC(3);
}

// *************************************************************************
// LDX
// *************************************************************************
W65C02_instruction_def(ldx_zp) {
  LOAD_ZP(x, ZP_ADDR(OP8()));
  INC_PC(2);
}


W65C02_instruction_def(ldx_abs) {
  LOAD(x, MEM_ADDR(OP16()));
  INC_PC(3);
}

W65C02_instruction_def(ldx_zp_y) {
  LOAD_ZP(x, INDEXED(OP8(), y));
  INC_PC(2);
}

W65C02_instruction_def(ldx_abs_y) {
  uint16_t base = OP16();
  PAGE_PENALTY(base, y);
  LOAD(x, INDEXED(base, y));
  INC_PC(3);
}

W65C02_instruction_def(ldx_immed) {
  uint8_t v = OP8();
  SET_FLAGS(interpreter->x = v);
  INC_PC(2);
}


// *************************************************************************
// LDY
// *************************************************************************

W65C02_instruction_def(ldy_zp) {
  LOAD_ZP(y, ZP_ADDR(OP8()));
  INC_PC(2);
}

W65C02_instruction_def(ldy_zp_x) {
  LOAD_ZP(y, INDEXED(OP8(), x));
  INC_PC(2);
}

W65C02_instruction_def(ldy_immed) {
  uint8_t v = OP8();
  SET_FLAGS(interpreter->y = v);
  INC_PC(2);
}

W65C02_instruction_def(ldy_abs) {
  LOAD(y, MEM_ADDR(OP16()));
  INC_PC(3);
}

W65C02_instruction_def(ldy_abs_x) {
  uint16_t base = OP16();
  PAGE_PENALTY(base, x);
  LOAD(y, INDEXED(base, x));
  INC_PC(3);
}

// *************************************************************************
// ADC
// *************************************************************************

W65C02_instruction_def(adc_zp_x_indirect) {
  ADD_WITH_CARRY(*INDEXED_INDIRECT(OP8(),x));
  INC_PC(2);
}

W65C02_instruction_def(adc_zp_s) {
  UNIMPLEMENTED(adc_zp_s);
}

W65C02_instruction_def(adc_zp) {
  ADD_WITH_CARRY(ZP(OP8()));
  INC_PC(2);
}

W65C02_instruction_def(adc_zp_y) {
  ADD_WITH_CARRY(*INDEXED(OP8(), y));
  INC_PC(2);
}

W65C02_instruction_def(adc_immed) {
  ADD_WITH_CARRY(OP8());
  INC_PC(2);
}

W65C02_instruction_def(adc_abs) {
  ADD_WITH_CARRY(W65C02ReadByte(interpreter, OP16()));
  INC_PC(3);
}

W65C02_instruction_def(adc_zp_indirect_y) {
  uint8_t zp = OP8();
  PAGE_PENALTY(*(uint16_t*)(interpreter->memory + zp), y);
  ADD_WITH_CARRY(*INDIRECT_INDEXED(zp, y));
  INC_PC(2);
}

W65C02_instruction_def(adc_zp_indirect) {
  ADD_WITH_CARRY(*INDIRECT_ADDR(OP8()));
  INC_PC(2);
}

W65C02_instruction_def(adc_zp_s_indirect_y) {
  UNIMPLEMENTED(adc_zp_s_indirect_y);
}

W65C02_instruction_def(adc_zp_x) {
  ADD_WITH_CARRY(*INDEXED(OP8(), x));
  INC_PC(2);
}

W65C02_instruction_def(adc_abs_y) {
  uint16_t base = OP16();
  PAGE_PENALTY(base, y);
  ADD_WITH_CARRY(*INDEXED(base, y));
  INC_PC(3);
}

W65C02_instruction_def(adc_abs_x) {
  uint16_t base = OP16();
  PAGE_PENALTY(base, x);
  ADD_WITH_CARRY(*INDEXED(base, x));
  INC_PC(3);
}


// *************************************************************************
//  SBC
// *************************************************************************

W65C02_instruction_def(sbc_zp_x_indirect) {
  SUB_WITH_BORROW(*INDEXED_INDIRECT(OP8(),x));
  INC_PC(2);
}

W65C02_instruction_def(sbc_zp_s) {
  UNIMPLEMENTED(sbc_zp_s);
}

W65C02_instruction_def(sbc_zp) {
  SUB_WITH_BORROW(ZP(OP8()));
  INC_PC(2);
}

W65C02_instruction_def(sbc_zp_indirect) {
  SUB_WITH_BORROW(*INDIRECT_ADDR(OP8()));
  INC_PC(2);
}

W65C02_instruction_def(sbc_immed) {
  SUB_WITH_BORROW(OP8());
  INC_PC(2);
}

W65C02_instruction_def(sbc_abs) {
  SUB_WITH_BORROW(W65C02ReadByte(interpreter, OP16()));
  INC_PC(3);
}

W65C02_instruction_def(sbc_zp_indirect_y) {
  uint8_t zp = OP8();
  PAGE_PENALTY(*(uint16_t*)(interpreter->memory + zp), y);
  SUB_WITH_BORROW(*INDIRECT_INDEXED(zp, y));
  INC_PC(2);
}

W65C02_instruction_def(sbc_zp_s_indirect_y) {
  UNIMPLEMENTED(sbc_zp_s_indirect_y);
}

W65C02_instruction_def(sbc_zp_x) {
  SUB_WITH_BORROW(*INDEXED(OP8(), x));
  INC_PC(2);
}


W65C02_instruction_def(sbc_abs_y) {
  uint16_t base = OP16();
  PAGE_PENALTY(base, y);
  SUB_WITH_BORROW(*INDEXED(base, y));
  INC_PC(3);
}

W65C02_instruction_def(sbc_abs_x) {
  uint16_t base = OP16();
  PAGE_PENALTY(base, x);
  SUB_WITH_BORROW(*INDEXED(base, x));
  INC_PC(3);
}

// *************************************************************************
// INC
// *************************************************************************

W65C02_instruction_def(inc_a) {
  SET_FLAGS(++interpreter->a);
  INC_PC(1);
}

W65C02_instruction_def(inc_zp) {
  uint16_t addr = OP8();
  SET_FLAGS(++(*ZP_ADDR(addr)));
  INC_PC(2);
}

W65C02_instruction_def(inc_abs) {
  uint16_t addr = OP16();
  SET_FLAGS(++(*MEM_ADDR(addr)));
  INC_PC(3);
}

W65C02_instruction_def(inc_zp_x) {
  uint8_t* p = INDEXED(OP8(),x);
  SET_FLAGS(++(*p));
  INC_PC(2);
}

W65C02_instruction_def(inc_abs_x) {
  uint8_t* p = INDEXED(OP16(),x);
  SET_FLAGS(++(*p));
  INC_PC(3);
}

W65C02_instruction_def(inx) {
  SET_FLAGS(++interpreter->x);
  INC_PC(1);
}

W65C02_instruction_def(iny) {
  SET_FLAGS(++interpreter->y);
  INC_PC(1);
}


// *************************************************************************
// DEC
// *************************************************************************

W65C02_instruction_def(dec_accum) {
  SET_FLAGS(--interpreter->a);
  INC_PC(1);
}

W65C02_instruction_def(dec_zp) {
  uint8_t* p = ZP_ADDR(OP8());
  SET_FLAGS(--(*p));
  INC_PC(2);
}

W65C02_instruction_def(dec_abs) {
  uint8_t* p = (uint8_t*)MEM_ADDR(OP16());
  SET_FLAGS(--(*p));
  INC_PC(3);
}

W65C02_instruction_def(dec_zp_x) {
  uint8_t* p = INDEXED(OP8(),x);
  SET_FLAGS(--(*p));
  INC_PC(2);
}

W65C02_instruction_def(dec_abs_x) {
  uint8_t* p = INDEXED(OP16(),x);
  SET_FLAGS(--(*p));
  INC_PC(3);
}

W65C02_instruction_def(dex) {
  SET_FLAGS(--interpreter->x);
  INC_PC(1);
}

W65C02_instruction_def(dey) {
  SET_FLAGS(--interpreter->y);
  INC_PC(1);
}

// *************************************************************************
// BIT
// *************************************************************************

W65C02_instruction_def(bit_zp) {
  uint16_t addr = OP8();
  int result = interpreter->a & ZP(addr);
  SET_SIGN(ZP(addr));
  SET_ZERO(result);
  SET_OVERFLOW(ZP(addr) & 0x40);
  INC_PC(2);
}

W65C02_instruction_def(bit_abs) {
  uint16_t addr = OP16();
  uint8_t mem = W65C02ReadByte(interpreter, addr);
  int result = interpreter->a & mem;
  SET_SIGN(mem);
  SET_ZERO(result);
  SET_OVERFLOW(mem & 0x40);
  INC_PC(3);
}

W65C02_instruction_def(bit_zp_x) {
  uint16_t addr = OP8();
  uint8_t* ptr = INDEXED(addr, x);
  int result = interpreter->a & *ptr;
  SET_SIGN(*ptr);
  SET_ZERO(result);
  SET_OVERFLOW(*ptr & 0x40);
  INC_PC(2);
}


W65C02_instruction_def(bit_abs_x) {
  uint16_t addr = OP16();
  PAGE_PENALTY(addr, x);
  uint8_t mem = W65C02ReadByte(interpreter, (uint16_t)(addr + interpreter->x));
  int result = interpreter->a & mem;
  SET_SIGN(mem);
  SET_ZERO(result);
  SET_OVERFLOW(mem & 0x40);
  INC_PC(3);
}

W65C02_instruction_def(bit_immed) {
  // BIT # only updates Z. N and V stay as they were.
  uint8_t value = OP8();
  SET_ZERO(interpreter->a & value);
  INC_PC(2);
}


// *************************************************************************
// CMP
// *************************************************************************

W65C02_instruction_def(cmp_zp_x_indirect) {
  uint16_t addr = OP8();
  uint8_t v = *INDEXED_INDIRECT(addr,x);
  uint16_t result = interpreter->a - v;
  SET_CMP_FLAGS(a, v, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cmp_zp_s) {
  
}

W65C02_instruction_def(cmp_zp) {
  uint8_t addr = OP8();
  uint8_t v = ZP(addr);
  uint16_t result = interpreter->a - v;
  SET_CMP_FLAGS(a, v, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cmp_zp_indirect) {
  uint8_t addr = OP8();
  int8_t v = *INDIRECT_ADDR(addr);
  uint16_t result = interpreter->a - v;
  SET_CMP_FLAGS(a, v, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cmp_immed) {
  uint8_t value = OP8();
  uint16_t result = interpreter->a - value;
  SET_CMP_FLAGS(a, value, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cmp_abs) {
  uint16_t addr = OP16();
  uint8_t v = W65C02ReadByte(interpreter, addr);
  uint16_t result = interpreter->a - v;
  SET_CMP_FLAGS(a, v, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(3);
}

W65C02_instruction_def(cmp_zp_indirect_y) {
  uint8_t addr = OP8();
  PAGE_PENALTY(*(uint16_t*)(interpreter->memory + addr), y);
  uint8_t v = *INDIRECT_INDEXED(addr,y);
  uint16_t result = interpreter->a - v;
  SET_CMP_FLAGS(a, v, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cmp_zp_s_indirect_y) {
  
}

W65C02_instruction_def(cmp_zp_x) {
  uint8_t addr = OP8();
  uint8_t* ptr = INDEXED(addr, x);
  uint16_t result = interpreter->a - *ptr;
  SET_CMP_FLAGS(a, *ptr, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cmp_abs_y) {
  uint16_t addr = OP16();
  PAGE_PENALTY(addr, y);
  uint8_t v = W65C02ReadByte(interpreter, (uint16_t)(addr + interpreter->y));
  uint16_t result = interpreter->a - v;
  SET_CMP_FLAGS(a, v, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(3);
}

W65C02_instruction_def(cmp_abs_x) {
  uint16_t addr = OP16();
  PAGE_PENALTY(addr, x);
  uint8_t v = W65C02ReadByte(interpreter, (uint16_t)(addr + interpreter->x));
  uint16_t result = interpreter->a - v;
  SET_CMP_FLAGS(a, v, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(3);
}



// *************************************************************************
// CPX
// *************************************************************************

W65C02_instruction_def(cpx_immed) {
  uint8_t value = OP8();
  uint16_t result = interpreter->x - value;
  SET_CMP_FLAGS(x, value, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cpx_zp) {
  uint8_t addr = OP8();
  uint8_t value = ZP(addr);
  uint16_t result = interpreter->x - value;
  SET_CMP_FLAGS(x, value, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cpx_abs) {
  uint16_t addr = OP16();
  uint8_t value = W65C02ReadByte(interpreter, addr);
  uint16_t result = interpreter->x - value;
  SET_CMP_FLAGS(x, value, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(3);
}



// *************************************************************************
// CPY
// *************************************************************************

W65C02_instruction_def(cpy_immed) {
  uint8_t value = OP8();
  uint16_t result = interpreter->y - value;
  SET_CMP_FLAGS(y, value, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cpy_zp) {
  uint8_t addr = OP8();
  uint8_t value = ZP(addr);
  uint16_t result = interpreter->y - value;
  SET_CMP_FLAGS(y, value, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(2);
}

W65C02_instruction_def(cpy_abs) {
  uint16_t addr = OP16();
  uint8_t value = W65C02ReadByte(interpreter, addr);
  uint16_t result = interpreter->y - value;
  SET_CMP_FLAGS(y, value, result);
  SET_CARRY_VALUE(result < 256);
  INC_PC(3);
}


// *************************************************************************
// Branches
// *************************************************************************

W65C02_instruction_def(bpl) {
  BRANCH(s, 0);
}

W65C02_instruction_def(bmi) {
  BRANCH(s, 1);
}

W65C02_instruction_def(bvc) {
  BRANCH(v, 0);
}

W65C02_instruction_def(bvs) {
  BRANCH(v, 1);
}

W65C02_instruction_def(bcc) {
  BRANCH(c, 0);
}

W65C02_instruction_def(bcs) {
  BRANCH(c, 1);
}

W65C02_instruction_def(bne) {
  BRANCH(z, 0);
}

W65C02_instruction_def(beq) {
  BRANCH(z, 1);
}

W65C02_instruction_def(bra) {
  int8_t offset = OP8();
  uint16_t base = (uint16_t)(interpreter->pc + 2);
  uint16_t dest = (uint16_t)(base + offset);
  if ((base & 0xff00) != (dest & 0xff00)) {
    interpreter->extra_cycles++;
  }
  interpreter->pc = dest;
}

// *************************************************************************
// Jumps
// *************************************************************************

W65C02_instruction_def(jmp_abs) {
  uint16_t addr = OP16();
  JMP(addr);
}

W65C02_instruction_def(jmp_abs_indirect) {
  uint16_t addr = OP16();
  // The NMOS 6502 fetches the high byte from the start of the page when the
  // pointer ends in $FF. The 65C02 reads the next page.
  if (interpreter->nmos && (addr & 0xff) == 0xff) {
    uint8_t lo = interpreter->memory[addr];
    uint8_t hi = interpreter->memory[(uint16_t)(addr & 0xff00)];
    JMP((uint16_t)(lo | (hi << 8)));
    return;
  }
  JMP(INDIRECT2(addr));
}

W65C02_instruction_def(jmp_abs_x_indirect) {
  uint16_t addr = (uint16_t)(OP16() + interpreter->x);
  JMP(INDIRECT2(addr));
}

W65C02_instruction_def(jsr_abs) {
  PUSH_PC(2);
  uint16_t addr = OP16();
  JMP(addr);
}

W65C02_instruction_def(jsr_abs_x_indirect) {
  PUSH_PC(2);
  uint16_t addr = OP16();
  JMP(*(uint16_t*)(INDEXED_INDIRECT(addr, x)));
}

W65C02_instruction_def(rts) {
  POP_PC(1);
}

W65C02_instruction_def(rti) {
  POP(flags.value);
  interpreter->flags.value &= ~(3 << 4);  // Clear bits 4 and 5.
  POP_PC(0);
  if (interpreter->in_irq > 0) {
    interpreter->in_irq--;
  }
}

// *************************************************************************
// STA
// *************************************************************************

W65C02_instruction_def(sta_zp_x_indirect) {
  STORE(a, INDEXED_INDIRECT_ADDR(OP8(), x));
  INC_PC(2);
}

W65C02_instruction_def(sta_zp_s) {
  
}

W65C02_instruction_def(sta_zp) {
  STORE_ZP(a, ZP_ADDR(OP8()));
  INC_PC(2);
}

W65C02_instruction_def(sta_abs) {
  STORE(a, MEM_ADDR(OP16()));
  INC_PC(3);
}

W65C02_instruction_def(sta_zp_indirect_y) {
  STORE(a, INDIRECT_INDEXED(OP8(), y));
  INC_PC(2);
}

W65C02_instruction_def(sta_zp_indirect) {
  uint16_t addr = OP8();
  STORE(a, INDIRECT_ADDR(addr));
  INC_PC(2);
}

W65C02_instruction_def(sta_zp_s_indirect_y) {
  STORE(a, INDIRECT_INDEXED(OP8(), y));
  INC_PC(2);
}

W65C02_instruction_def(sta_zp_x) {
  uint16_t addr = OP8();
  STORE_ZP(a, INDEXED(addr, x));
  INC_PC(2);
}

W65C02_instruction_def(sta_zp_y) {
  uint16_t addr = OP8();
  STORE_ZP(a, INDEXED(addr, y));
  INC_PC(2);
}

W65C02_instruction_def(sta_abs_y) {
  uint16_t addr = OP16();
  STORE(a, INDEXED(addr, y));
  INC_PC(3);
}

W65C02_instruction_def(sta_abs_x) {
  uint16_t addr = OP16();
  STORE(a, INDEXED(addr, x));
  INC_PC(3);
}

// *************************************************************************
// STX
// *************************************************************************

W65C02_instruction_def(stx_zp) {
  STORE_ZP(x, ZP_ADDR(OP8()));
  INC_PC(2);
}

W65C02_instruction_def(stx_abs) {
  STORE(x, MEM_ADDR(OP16()));
  INC_PC(3);
}

W65C02_instruction_def(stx_zp_y) {
  uint16_t addr = OP8();
  STORE(x, INDEXED(addr, y));
  INC_PC(2);
}

// *************************************************************************
// STY
// *************************************************************************

W65C02_instruction_def(sty_zp_x) {
  uint16_t addr = OP8();
  STORE_ZP(y, INDEXED(addr, x));
  INC_PC(2);
}

W65C02_instruction_def(sty_abs) {
  STORE(y, MEM_ADDR(OP16()));
  INC_PC(3);
}

W65C02_instruction_def(sty_zp) {
  STORE_ZP(y, ZP_ADDR(OP8()));
  INC_PC(2);
}

// *************************************************************************
// STZ
// *************************************************************************

W65C02_instruction_def(stz_zp) {
  STORE_ZERO(ZP_ADDR(OP8()));
  INC_PC(2);
}

W65C02_instruction_def(stz_zp_x) {
  uint16_t addr = OP8();
  STORE_ZERO(INDEXED(addr, x));
  INC_PC(2);
}

W65C02_instruction_def(stz_abs) {
  STORE_ZERO(MEM_ADDR(OP16()));
  INC_PC(3);
}

W65C02_instruction_def(stz_abs_x) {
  uint16_t addr = OP16();
  STORE_ZERO(INDEXED(addr, x));
  INC_PC(3);
}

// *************************************************************************
// Flags
// *************************************************************************

W65C02_instruction_def(clc) {
  interpreter->flags.bits.c = 0;
  INC_PC(1);
}

W65C02_instruction_def(sec) {
  interpreter->flags.bits.c = 1;
  INC_PC(1);
}

W65C02_instruction_def(cli) {
  interpreter->flags.bits.i = 0;
  INC_PC(1);
}

W65C02_instruction_def(sei) {
  interpreter->flags.bits.i = 1;
  INC_PC(1);
}

W65C02_instruction_def(clv) {
  interpreter->flags.bits.v = 0;
  INC_PC(1);
}

W65C02_instruction_def(cld) {
  interpreter->flags.bits.d = 0;
  INC_PC(1);
}

W65C02_instruction_def(sed) {
  interpreter->flags.bits.d = 1;
  INC_PC(1);
}

// *************************************************************************
// Stack
// *************************************************************************

W65C02_instruction_def(php) {
  PUSH(interpreter->flags.value | (3 << 4));  // Bits 4 and 5 set.
  INC_PC(1);
}

W65C02_instruction_def(plp) {
  POP(flags.value);
  interpreter->flags.value &= ~(3 << 4);  // Clear bits 4 and 5.
  INC_PC(1);
}

W65C02_instruction_def(pha) {
  PUSH(interpreter->a);
  INC_PC(1);
}

W65C02_instruction_def(phy) {
  PUSH(interpreter->y);
  INC_PC(1);
}

W65C02_instruction_def(ply) {
  POP(y);
  INC_PC(1);
  SET_FLAGS(interpreter->y);
}

W65C02_instruction_def(pla) {
  POP(a);
  INC_PC(1);
  SET_FLAGS(interpreter->a);
}

W65C02_instruction_def(phx) {
  PUSH(interpreter->x);
  INC_PC(1);
}

W65C02_instruction_def(plx) {
  POP(x);
  INC_PC(1);
  SET_FLAGS(interpreter->x);
}

// *************************************************************************
// Register transfers
// *************************************************************************


W65C02_instruction_def(tsx) {
  SET_FLAGS(interpreter->x = interpreter->s);
  INC_PC(1);
}
W65C02_instruction_def(tyx) {
  SET_FLAGS(interpreter->y = interpreter->x);
  INC_PC(1);
}

W65C02_instruction_def(txa) {
  SET_FLAGS(interpreter->a = interpreter->x);
  INC_PC(1);
}

W65C02_instruction_def(tya) {
  SET_FLAGS(interpreter->a = interpreter->y);
  INC_PC(1);
}

W65C02_instruction_def(tay) {
  SET_FLAGS(interpreter->y = interpreter->a);
  INC_PC(1);
}


W65C02_instruction_def(tax) {
  SET_FLAGS(interpreter->x = interpreter->a);
  INC_PC(1);
}

W65C02_instruction_def(txs) {
  interpreter->s = interpreter->x;
  // TXS does not change the flags. The davecc CMOS core historically did.
  if (!interpreter->sc12) {
    SET_FLAGS(interpreter->s);
  }
  INC_PC(1);
}

W65C02_instruction_def(txy) {
  SET_FLAGS(interpreter->y = interpreter->x);
  INC_PC(1);
}


// *************************************************************************
// Additional
// *************************************************************************

// Software break.  Called from hardware BRK handler.  On entry we have
// A = value of byte after BRK instruction.
W65C02_instruction_def(sbrk) {
  // Interpreted 6502 BRK.
  INC_PC(1);            // Move PC to byte after BRK.
  BrkHandler(interpreter, interpreter->a);
}

W65C02_instruction_def(bpt) {
  // Uninstall the current breakpoint.
  bool temp_bp = false;
  Breakpoint* bp = FindBreakpoint(interpreter, interpreter->pc);
  interpreter->current_bp = bp;
  if (bp == NULL) {
    abort();
  }
  if (bp->is_temp) {
    // Hit temp s: remove it, step one instruction and continue
    // with main loop.
    BreakpointUninstall(interpreter, bp);
    RemoveBreakpoint(interpreter, bp);
    temp_bp = true;
  } else {
    printf("Stopped at breakpoint set at address 0x%x\n", interpreter->pc);
    BreakpointUninstall(interpreter, bp);
  }
  
  // Enter debugger command loop.  We will exit on a continue or step
  // instruction.
  DebuggerLoop(interpreter);
  
  // Move on from the current instruction.
  StepOneInstruction(interpreter, false);
  
  // Reinstall the breakpoint.
  if (!temp_bp) {
    BreakpointInstall(interpreter, bp);
  }
  interpreter->current_bp = NULL;
}

W65C02_instruction_def(cop_zp) {
  
}

W65C02_instruction_def(tsb_zp) {
  TestAndChangeBits(interpreter, OP8(), true);
  INC_PC(2);
}

W65C02_instruction_def(tsb_abs) {
  TestAndChangeBits(interpreter, OP16(), true);
  INC_PC(3);
}

W65C02_instruction_def(phd) {
  
}

W65C02_instruction_def(trb_zp) {
  TestAndChangeBits(interpreter, OP8(), false);
  INC_PC(2);
}

W65C02_instruction_def(tcs) {
  
}
W65C02_instruction_def(trb_abs) {
  TestAndChangeBits(interpreter, OP16(), false);
  INC_PC(3);
}

W65C02_instruction_def(pld) {
  
}


W65C02_instruction_def(tsc) {
  
}

W65C02_instruction_def(wdm) {
  
}

W65C02_instruction_def(mvp_s_zp) {
  
}


W65C02_instruction_def(phk) {
  
}


W65C02_instruction_def(mvn_s_zp) {
  
}



W65C02_instruction_def(tcd) {
  
}



W65C02_instruction_def(per) {
  
}



W65C02_instruction_def(rtl) {
  
}





W65C02_instruction_def(tdc) {
  
}

W65C02_instruction_def(brl) {
  
}

W65C02_instruction_def(phb) {
  
}


W65C02_instruction_def(plb) {
  
}

W65C02_instruction_def(rep_immed) {
  
}

W65C02_instruction_def(wal) {
  
}

W65C02_instruction_def(pei_zp) {
  
}

W65C02_instruction_def(stp) {
  
}

W65C02_instruction_def(jml_abs_indirect) {
  
}

W65C02_instruction_def(sep_immed) {
  
}

W65C02_instruction_def(xba) {
  
}

W65C02_instruction_def(pea_abs) {
  
}

W65C02_instruction_def(xce) {
  
}
