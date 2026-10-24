//
//  6502_interpreter.c
//  6502_interpreter
//
//  Created by David Allison on 5/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include "6502_interpreter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "6502_disassembler.h"
#include "6502_devices.h"
#include "bbc_hardware.h"
#include "6502_nmos.h"
#include <time.h>

#ifdef __MACH__
#include <mach/mach_time.h> /* mach_absolute_time */
#endif

static void BbcSyncIrqFromVideo(void* ctx);
static int StepOneInstruction(W65C02Interpreter* interpreter, bool cycle_accurate);

#define W65C02_instruction_decl(x) static void Interpret_##x(W65C02Interpreter*);
#define W65C02_instruction_def(x) static void Interpret_##x(W65C02Interpreter* interpreter)
#define W65C02_instruction(x) Interpret_##x

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
  {W65C02_instruction(nop), 1, 2},
  
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
                           bool cycle_accurate, bool trace) {
  memset(interpreter, 0, sizeof(W65C02Interpreter));
  interpreter->debug = debug;
  interpreter->current_bp = NULL;
  interpreter->next_bp_num = 1;
  interpreter->trace = trace;
  interpreter->cycle_accurate = cycle_accurate;
  interpreter->allow_turbo = true;
  VectorInit(&interpreter->breakpoints);
  VectorInit(&interpreter->watchpoints);
  VectorInit(&interpreter->devices);
  interpreter->bbc_initial_mode = -1;
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
    BbcMachineSetIrqSync(interpreter->bbc, BbcSyncIrqFromVideo, interpreter);
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

bool W65C02InterpreterBbcLoadHardDisc(W65C02Interpreter* interpreter, const char* path) {
  if (interpreter->bbc == NULL || path == NULL) {
    return false;
  }
  return BbcMachineLoadHardDisc(interpreter->bbc, path);
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

void W65C02DisassemblePc(W65C02Interpreter* interpreter) {
  Breakpoint* bp = FindBreakpoint(interpreter, interpreter->pc);
  if (bp != NULL) {
    BreakpointUninstall(interpreter, bp);
  }
  Disassemble6502Instruction(interpreter->pc, &interpreter->memory[interpreter->pc], stdout);
  if (bp != NULL) {
    BreakpointInstall(interpreter, bp);
  }
}

// 2 MHz = 500 ns per cycle.
#define CPU_CYCLE_NS 500

bool W65C02InterpreterSetClockMhz(W65C02Interpreter* interpreter, int mhz) {
  if (interpreter == NULL || mhz < 1 || mhz > 16) {
    return false;
  }
  interpreter->clock_mhz = mhz;
  interpreter->allow_turbo = false;
  interpreter->turbo = false;
  return true;
}

// Nanoseconds of wall time for one guest cycle. A fixed clock above 2 MHz
// shortens this so the whole machine, timers included, runs faster together.
static uint64_t NsPerCycle(const W65C02Interpreter* interpreter) {
  int mhz = interpreter->clock_mhz;
  uint64_t ns;
  if (mhz < 1) {
    mhz = 2;
  }
  ns = (uint64_t)CPU_CYCLE_NS * 2 / (uint64_t)mhz;
  if (ns < 1) {
    ns = 1;
  }
  return ns;
}

#ifdef __MACH__
static uint64_t TicksForNs(uint64_t ns) {
  static mach_timebase_info_data_t info;
  static int ready = 0;
  uint64_t ticks;
  if (!ready) {
    mach_timebase_info(&info);
    ready = 1;
  }
  ticks = (ns * info.denom + info.numer / 2) / info.numer;
  if (ticks == 0) {
    ticks = 1;
  }
  return ticks;
}
#endif

// A short stretch with interrupts on and almost no stores outside page 0 is
// the keyboard wait, a BASIC delay, or a poll of the CRTC or system VIA. A
// disc poll reads the controller instead, and a memory fill stores constantly:
// those run ahead.
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
  // Split-screen and beam-ULA games keep timer 1 aligned with the raster. Turbo
  // skips the beam and breaks that, which shows as scrolling HUD garbage.
  if (BbcMachineNeedsRealtimeVideo(interpreter->bbc)) {
    interpreter->turbo = false;
    interpreter->pace_busy = 0;
    interpreter->pace_cycles = 0;
    interpreter->pace_stores = 0;
    interpreter->pace_sheila = 0;
    interpreter->pace_watch = 0;
    return;
  }
  // A game that writes the screen every pass still polls the CRTC or the
  // system VIA for vsync. Do not run ahead in that window or the MOS keyboard
  // scan and split-screen games such as Revs lose time with the hardware.
  waiting = !interpreter->flags.bits.i &&
            (interpreter->pace_watch > 0 || interpreter->pace_stores < 48);
  // A cursor blink or one disc poll looks like work for a single window.
  // Drop to real time immediately, and only run ahead after the guest has
  // stayed busy, so the frame size does not flap.
  if (!interpreter->allow_turbo || waiting || BbcMachineSounding(interpreter->bbc)) {
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
    if (fast && BbcMachineNeedsRealtimeVideo(interpreter->bbc)) {
      fast = false;
      interpreter->turbo = false;
      interpreter->pace_busy = 0;
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
  {
    uint64_t ticks = TicksForNs(NsPerCycle(interpreter));
    target = interpreter->cycle_anchor + interpreter->cycle_origin * ticks;
    if (target > now) {
      mach_wait_until(target);
    } else if (now - target > TicksForNs(50000000ull)) {
      interpreter->cycle_anchor = now;
      interpreter->cycle_origin = 0;
    }
  }
#else
  target = interpreter->cycle_anchor + interpreter->cycle_origin * NsPerCycle(interpreter);
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
  // Page 0 is the BASIC scratchpad and page 1 is the stack. A delay such as
  // UNTIL TIME MOD 100=0 stores there on every pass. Counting those stores
  // makes the pause look like a memory fill, so the clock runs fast and the
  // beam is never painted.
  if (cpu->in_irq || addr < 0x200) {
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

// A guest that polls the system VIA interrupt flag has to observe the bit
// the hardware just raised. The interrupt is taken after that instruction,
// so the operating system's handler does not clear the flag first.
static int MaybeTakeIrq(W65C02Interpreter* interpreter) {
  if (interpreter->flags.bits.i) {
    return 0;
  }
  if (interpreter->bbc != NULL && BbcMachineIrqPending(interpreter->bbc)) {
    if (BbcMachineDeferUserViaT1Irq(interpreter->bbc)) {
      return 0;
    }
    W65C02TakeIrq(interpreter);
    return 7;
  }
  if (interpreter->irq_pending != NULL && interpreter->irq_pending(interpreter->irq_ctx)) {
    W65C02TakeIrq(interpreter);
    return 7;
  }
  return 0;
}

static void BbcSyncIrqFromVideo(void* ctx) {
  W65C02Interpreter* interpreter = ctx;
  BbcMachine* bbc;
  int guard;
  int depth;
  if (interpreter == NULL || interpreter->bbc == NULL || interpreter->flags.bits.i) {
    return;
  }
  bbc = interpreter->bbc;
  if (!BbcMachineUserViaT1Pending(bbc) && !BbcMachineSysViaT1Pending(bbc)) {
    return;
  }
  W65C02TakeIrq(interpreter);
  depth = interpreter->in_irq;
  BbcMachineSetVideoIrqDispatch(bbc, true);
  for (guard = 0; guard < 4096; guard++) {
    if (interpreter->in_irq < depth) {
      break;
    }
    StepOneInstruction(interpreter, true);
  }
  BbcMachineSetVideoIrqDispatch(bbc, false);
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
  if (interpreter->nmi_pending != NULL && interpreter->nmi_pending(interpreter->irq_ctx)) {
    if (interpreter->nmi_clear != NULL) {
      interpreter->nmi_clear(interpreter->irq_ctx);
    }
    W65C02TakeNmi(interpreter);
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
  // The other unused opcodes are multi-byte NOPs. Rockwell BBR/RMB are not
  // part of this CPU.
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
      nop_cycles += MaybeTakeIrq(interpreter);
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
      cycles += MaybeTakeIrq(interpreter);
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
  cycles += MaybeTakeIrq(interpreter);
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
  interpreter->turbo = interpreter->allow_turbo && interpreter->bbc_os_path != NULL;
  interpreter->in_irq = 0;
  interpreter->jammed = false;
  interpreter->pc = (uint16_t)(interpreter->memory[0xfffc] |
                               (interpreter->memory[0xfffd] << 8));
  interpreter->running = true;
  return true;
}

int W65C02InterpreterStepRaw(W65C02Interpreter* interpreter) {
  if (interpreter == NULL || !interpreter->running) {
    return 0;
  }
  return StepOneInstruction(interpreter, false);
}

int W65C02InterpreterStep(W65C02Interpreter* interpreter) {
  int cycles;
  if (!interpreter->running) {
    return 0;
  }
  cycles = StepOneInstruction(interpreter, true);
  if (cycles > 0 && interpreter->bbc != NULL) {
    BbcMachineRunParasite(interpreter->bbc, cycles);
  }
  return cycles;
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
  interpreter->turbo = interpreter->allow_turbo && interpreter->bbc_os_path != NULL;
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

void W65C02InterpreterDestruct(W65C02Interpreter* interpreter) {
  size_t i;
  if (interpreter->bbc_ram_path != NULL && interpreter->bbc_ram_path[0] != '\0' &&
      interpreter->memory != NULL) {
    FILE* ram = fopen(interpreter->bbc_ram_path, "wb");
    if (ram == NULL || fwrite(interpreter->memory, 1, 65536, ram) != 65536) {
      fprintf(stderr, "Unable to write BBC RAM to '%s'\n", interpreter->bbc_ram_path);
    }
    if (ram != NULL) {
      fclose(ram);
    }
  }
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
  if (interpreter->write_protect != 0 && addr >= interpreter->write_protect) {
    return;
  }
  if (!BbcMachineSidewaysRom(interpreter->bbc, addr) &&
      !BbcMachineMosRom(interpreter->bbc, addr)) {
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


// N is bit 7 of the 8-bit difference. V is not changed. A signed compare
// is N xor V, which is the caller's job; folding it into N makes BPL fire
// when the register is less than the operand.
#define SET_CMP_FLAGS(reg, src, result) {\
  uint8_t _v = (uint8_t)(result);\
  (void)(interpreter->reg);\
  (void)(src);\
  SET_ZERO(_v);\
  SET_SIGN(_v);\
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
    if (!BbcMachineSidewaysRom(interpreter->bbc, (uint16_t)((addr) - interpreter->memory)) && \
        !BbcMachineMosRom(interpreter->bbc, (uint16_t)((addr) - interpreter->memory))) { \
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
    if (!BbcMachineSidewaysRom(interpreter->bbc, (uint16_t)((addr) - interpreter->memory)) && \
        !BbcMachineMosRom(interpreter->bbc, (uint16_t)((addr) - interpreter->memory))) { \
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
  INC_PC(1);
}

W65C02_instruction_def(txy) {
  SET_FLAGS(interpreter->y = interpreter->x);
  INC_PC(1);
}


// *************************************************************************
// Additional
// *************************************************************************

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
