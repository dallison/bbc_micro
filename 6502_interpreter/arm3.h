//
//  arm3.h
//  ARM3 (ARMv2a) interpreter. Addresses are 26 bits. N, Z, C, V, the
//  interrupt masks, and the mode live in R15. There is no separate CPSR.
//

#ifndef ARM3_H
#define ARM3_H

#include <stdbool.h>
#include <stdint.h>

#define ARM3_N 0x80000000u
#define ARM3_Z 0x40000000u
#define ARM3_C 0x20000000u
#define ARM3_V 0x10000000u
#define ARM3_I 0x08000000u
#define ARM3_F 0x04000000u
#define ARM3_PC_MASK 0x03FFFFFCu
#define ARM3_ADDR_MASK 0x03FFFFFFu

#define ARM3_MODE_USR 0
#define ARM3_MODE_FIQ 1
#define ARM3_MODE_IRQ 2
#define ARM3_MODE_SVC 3

typedef struct Arm3 Arm3;

// The load and store hooks see a 26-bit address. Word hooks are called
// with the low two bits clear. A NULL hook reads as zero and ignores writes.
Arm3* Arm3Create(void);
void Arm3Destroy(Arm3* cpu);
void Arm3Reset(Arm3* cpu);
void Arm3SetMemory(Arm3* cpu,
                   uint32_t (*load32)(void* ctx, uint32_t addr),
                   void (*store32)(void* ctx, uint32_t addr, uint32_t value),
                   uint8_t (*load8)(void* ctx, uint32_t addr),
                   void (*store8)(void* ctx, uint32_t addr, uint8_t value),
                   void* ctx);
// irq and fiq are level checks. fiq_ack runs when a FIQ is taken, which
// is what an edge-triggered line needs so it does not fire again.
void Arm3SetIrq(Arm3* cpu,
                bool (*irq)(void* ctx),
                bool (*fiq)(void* ctx),
                void (*fiq_ack)(void* ctx));
// Cycles the instruction used. Zero means the CPU pointer was null.
int Arm3Step(Arm3* cpu);
// Register 15 is the current instruction address with the flags and mode.
// An instruction that reads R15 sees this address plus 8.
uint32_t Arm3Reg(const Arm3* cpu, int reg);
void Arm3SetReg(Arm3* cpu, int reg, uint32_t value);
uint32_t Arm3Pc(const Arm3* cpu);

#endif
