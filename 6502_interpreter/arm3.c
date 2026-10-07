//
//  arm3.c
//  ARM3 interpreter for the Tube second processor.
//
//  The arithmetic, the barrel shifter, and the condition codes follow the
//  ARM interpreter in ../c_compiler/arm_interpreter. That one is a hosted
//  32-bit ARM with a separate status register. This one is the 26-bit ARM3:
//  the address bus is 26 bits, and N, Z, C, V, the IRQ mask, the FIQ mask,
//  and the mode are the top and bottom bits of R15. SWP is included. Later
//  ARM instructions are undefined.
//

#include "arm3.h"

#include <stdlib.h>
#include <string.h>

struct Arm3 {
  uint32_t r[16];
  uint32_t bank_fiq[7];
  uint32_t bank_irq[2];
  uint32_t svc[2];
  uint32_t pc;
  bool branched;
  uint32_t (*load32)(void* ctx, uint32_t addr);
  void (*store32)(void* ctx, uint32_t addr, uint32_t value);
  uint8_t (*load8)(void* ctx, uint32_t addr);
  void (*store8)(void* ctx, uint32_t addr, uint8_t value);
  bool (*irq)(void* ctx);
  bool (*fiq)(void* ctx);
  void (*fiq_ack)(void* ctx);
  void* mem;
};

static void SwapWords(uint32_t* a, uint32_t* b, int count) {
  int i;
  for (i = 0; i < count; i++) {
    uint32_t tmp = a[i];
    a[i] = b[i];
    b[i] = tmp;
  }
}

static void SwapMode(Arm3* cpu, int mode) {
  if (mode == ARM3_MODE_FIQ) {
    SwapWords(cpu->r + 8, cpu->bank_fiq, 7);
  } else if (mode == ARM3_MODE_IRQ) {
    SwapWords(cpu->r + 13, cpu->bank_irq, 2);
  } else if (mode == ARM3_MODE_SVC) {
    SwapWords(cpu->r + 13, cpu->svc, 2);
  }
}

static void ChangeMode(Arm3* cpu, int mode) {
  int old = (int)(cpu->r[15] & 3u);
  mode &= 3;
  if (old == mode) {
    return;
  }
  SwapMode(cpu, old);
  SwapMode(cpu, mode);
  cpu->r[15] = (cpu->r[15] & ~3u) | (uint32_t)mode;
}

static void Take(Arm3* cpu, int mode, uint32_t vector, uint32_t ret, bool set_i, bool set_f) {
  uint32_t psr = cpu->r[15] & 0xFC000003u;
  ChangeMode(cpu, mode);
  cpu->r[14] = (ret & ARM3_PC_MASK) | psr;
  if (set_i) {
    cpu->r[15] |= ARM3_I;
  }
  if (set_f) {
    cpu->r[15] |= ARM3_F;
  }
  cpu->pc = vector & ARM3_PC_MASK;
  cpu->branched = true;
}

static uint32_t ReadReg(const Arm3* cpu, int reg, int ahead) {
  if (reg != 15) {
    return cpu->r[reg];
  }
  return ((cpu->pc + (uint32_t)ahead) & ARM3_PC_MASK) | (cpu->r[15] & ~ARM3_PC_MASK);
}

// An address taken from R15 is the instruction address plus 8. The flags
// and the mode are not part of that address, or a supervisor LDR [PC]
// would be unaligned.
static uint32_t ReadAddr(const Arm3* cpu, int reg) {
  if (reg == 15) {
    return (cpu->pc + 8u) & ARM3_PC_MASK;
  }
  return cpu->r[reg];
}

static uint32_t ReadUser(const Arm3* cpu, int reg) {
  int mode = (int)(cpu->r[15] & 3u);
  if (reg == 15) {
    return ReadReg(cpu, 15, 8);
  }
  if (reg < 8) {
    return cpu->r[reg];
  }
  if (reg < 13) {
    return mode == ARM3_MODE_FIQ ? cpu->bank_fiq[reg - 8] : cpu->r[reg];
  }
  if (mode == ARM3_MODE_FIQ) {
    return cpu->bank_fiq[reg - 8];
  }
  if (mode == ARM3_MODE_IRQ) {
    return cpu->bank_irq[reg - 13];
  }
  if (mode == ARM3_MODE_SVC) {
    return cpu->svc[reg - 13];
  }
  return cpu->r[reg];
}

static void WriteUser(Arm3* cpu, int reg, uint32_t value) {
  int mode = (int)(cpu->r[15] & 3u);
  if (reg == 15) {
    return;
  }
  if (reg < 8) {
    cpu->r[reg] = value;
    return;
  }
  if (reg < 13) {
    if (mode == ARM3_MODE_FIQ) {
      cpu->bank_fiq[reg - 8] = value;
    } else {
      cpu->r[reg] = value;
    }
    return;
  }
  if (mode == ARM3_MODE_FIQ) {
    cpu->bank_fiq[reg - 8] = value;
  } else if (mode == ARM3_MODE_IRQ) {
    cpu->bank_irq[reg - 13] = value;
  } else if (mode == ARM3_MODE_SVC) {
    cpu->svc[reg - 13] = value;
  } else {
    cpu->r[reg] = value;
  }
}

static void WriteRd(Arm3* cpu, int rd, uint32_t value, bool psr) {
  int mode;
  if (rd != 15) {
    cpu->r[rd] = value;
    return;
  }
  cpu->pc = value & ARM3_PC_MASK;
  cpu->branched = true;
  if (!psr) {
    return;
  }
  mode = (int)(cpu->r[15] & 3u);
  if (mode == ARM3_MODE_USR) {
    cpu->r[15] = (cpu->r[15] & ~0xF0000000u) | (value & 0xF0000000u);
    return;
  }
  ChangeMode(cpu, (int)(value & 3u));
  cpu->r[15] = (cpu->r[15] & ~0xFC000000u) | (value & 0xFC000000u);
}

static void SetNZ(Arm3* cpu, uint32_t value) {
  cpu->r[15] &= ~(ARM3_N | ARM3_Z);
  if ((value & 0x80000000u) != 0) {
    cpu->r[15] |= ARM3_N;
  }
  if (value == 0) {
    cpu->r[15] |= ARM3_Z;
  }
}

static void SetBit(Arm3* cpu, uint32_t bit, bool set) {
  if (set) {
    cpu->r[15] |= bit;
  } else {
    cpu->r[15] &= ~bit;
  }
}

static bool Condition(const Arm3* cpu, uint32_t insn) {
  bool n = (cpu->r[15] & ARM3_N) != 0;
  bool z = (cpu->r[15] & ARM3_Z) != 0;
  bool c = (cpu->r[15] & ARM3_C) != 0;
  bool v = (cpu->r[15] & ARM3_V) != 0;
  switch (insn >> 28) {
    case 0:
      return z;
    case 1:
      return !z;
    case 2:
      return c;
    case 3:
      return !c;
    case 4:
      return n;
    case 5:
      return !n;
    case 6:
      return v;
    case 7:
      return !v;
    case 8:
      return c && !z;
    case 9:
      return !c || z;
    case 10:
      return n == v;
    case 11:
      return n != v;
    case 12:
      return !z && n == v;
    case 13:
      return z || n != v;
    case 14:
      return true;
    default:
      return false;
  }
}

static uint32_t BusLoad32(Arm3* cpu, uint32_t addr) {
  addr &= ARM3_PC_MASK;
  if (cpu->load32 == NULL) {
    return 0;
  }
  return cpu->load32(cpu->mem, addr);
}

static void BusStore32(Arm3* cpu, uint32_t addr, uint32_t value) {
  addr &= ARM3_PC_MASK;
  if (cpu->store32 != NULL) {
    cpu->store32(cpu->mem, addr, value);
  }
}

static uint8_t BusLoad8(Arm3* cpu, uint32_t addr) {
  addr &= ARM3_ADDR_MASK;
  if (cpu->load8 == NULL) {
    return 0;
  }
  return cpu->load8(cpu->mem, addr);
}

static void BusStore8(Arm3* cpu, uint32_t addr, uint8_t value) {
  addr &= ARM3_ADDR_MASK;
  if (cpu->store8 != NULL) {
    cpu->store8(cpu->mem, addr, value);
  }
}

static uint32_t Shift(Arm3* cpu, uint32_t value, uint32_t type, uint32_t amount,
                      bool reg_specified, bool* carry_valid, bool* carry) {
  *carry_valid = false;
  *carry = (cpu->r[15] & ARM3_C) != 0;
  if (!reg_specified && amount == 0 && type != 0) {
    if (type == 1) {
      *carry_valid = true;
      *carry = (value >> 31) != 0;
      return 0;
    }
    if (type == 2) {
      *carry_valid = true;
      *carry = (value >> 31) != 0;
      return (value & 0x80000000u) != 0 ? 0xFFFFFFFFu : 0;
    }
    *carry_valid = true;
    *carry = (value & 1u) != 0;
    return (value >> 1) | (((cpu->r[15] & ARM3_C) != 0 ? 1u : 0u) << 31);
  }
  if (amount == 0) {
    return value;
  }
  if (type == 0) {
    if (amount >= 32) {
      *carry_valid = true;
      *carry = amount == 32 && (value & 1u) != 0;
      return 0;
    }
    *carry_valid = true;
    *carry = ((value >> (32 - amount)) & 1u) != 0;
    return value << amount;
  }
  if (type == 1) {
    if (amount >= 32) {
      *carry_valid = true;
      *carry = amount == 32 && (value >> 31) != 0;
      return 0;
    }
    *carry_valid = true;
    *carry = ((value >> (amount - 1)) & 1u) != 0;
    return value >> amount;
  }
  if (type == 2) {
    if (amount >= 32) {
      *carry_valid = true;
      *carry = (value >> 31) != 0;
      return (value & 0x80000000u) != 0 ? 0xFFFFFFFFu : 0;
    }
    *carry_valid = true;
    *carry = ((value >> (amount - 1)) & 1u) != 0;
    return (uint32_t)((int32_t)value >> amount);
  }
  if ((amount & 31u) == 0) {
    *carry_valid = true;
    *carry = (value >> 31) != 0;
    return value;
  }
  amount &= 31u;
  *carry_valid = true;
  *carry = ((value >> (amount - 1)) & 1u) != 0;
  return (value >> amount) | (value << (32 - amount));
}

static uint32_t ImmOperand(uint32_t insn, bool* carry_valid, bool* carry) {
  uint32_t imm = insn & 0xFFu;
  uint32_t rot = ((insn >> 8) & 0xFu) * 2u;
  uint32_t result;
  *carry_valid = false;
  *carry = false;
  if (rot == 0) {
    return imm;
  }
  result = (imm >> rot) | (imm << (32u - rot));
  *carry_valid = true;
  *carry = (result & 0x80000000u) != 0;
  return result;
}

static uint32_t RegOperand(Arm3* cpu, uint32_t insn, int ahead, bool* carry_valid, bool* carry) {
  int rm = (int)(insn & 15u);
  uint32_t type = (insn >> 5) & 3u;
  bool reg_shift = (insn & (1u << 4)) != 0;
  uint32_t amount;
  uint32_t value = ReadReg(cpu, rm, ahead);
  if (reg_shift) {
    int rs = (int)((insn >> 8) & 15u);
    amount = ReadReg(cpu, rs, ahead) & 0xFFu;
  } else {
    amount = (insn >> 7) & 31u;
  }
  return Shift(cpu, value, type, amount, reg_shift, carry_valid, carry);
}

static uint32_t Add(Arm3* cpu, uint32_t a, uint32_t b, uint32_t cin, bool flags) {
  uint64_t wide = (uint64_t)a + b + cin;
  uint32_t result = (uint32_t)wide;
  if (flags) {
    SetNZ(cpu, result);
    SetBit(cpu, ARM3_C, wide > 0xFFFFFFFFu);
    SetBit(cpu, ARM3_V, ((a ^ result) & (b ^ result) & 0x80000000u) != 0);
  }
  return result;
}

static uint32_t Sub(Arm3* cpu, uint32_t a, uint32_t b, uint32_t cin, bool flags) {
  uint32_t borrow = cin != 0 ? 0u : 1u;
  uint64_t wide = (uint64_t)a - b - borrow;
  uint32_t result = (uint32_t)wide;
  if (flags) {
    SetNZ(cpu, result);
    SetBit(cpu, ARM3_C, wide <= 0xFFFFFFFFull);
    SetBit(cpu, ARM3_V, ((a ^ b) & (a ^ result) & 0x80000000u) != 0);
  }
  return result;
}

static void LogicFlags(Arm3* cpu, uint32_t result, bool carry_valid, bool carry) {
  SetNZ(cpu, result);
  if (carry_valid) {
    SetBit(cpu, ARM3_C, carry);
  }
}

static int ExecuteData(Arm3* cpu, uint32_t insn) {
  int op = (int)((insn >> 21) & 15u);
  bool s = (insn & (1u << 20)) != 0;
  int rn = (int)((insn >> 16) & 15u);
  int rd = (int)((insn >> 12) & 15u);
  bool immediate = (insn & (1u << 25)) != 0;
  bool compare = op >= 8 && op <= 11;
  bool write_flags;
  bool restore;
  bool carry_valid = false;
  bool carry = false;
  int ahead = 8;
  int cycles = 1;
  uint32_t rhs;
  uint32_t lhs = 0;
  uint32_t result = 0;
  uint32_t cin;
  if (compare && !s) {
    Take(cpu, ARM3_MODE_SVC, 0x04, cpu->pc + 4, true, false);
    return 4;
  }
  if (!immediate && (insn & (1u << 4)) != 0) {
    ahead = 12;
    cycles = 2;
  }
  if (immediate) {
    rhs = ImmOperand(insn, &carry_valid, &carry);
  } else {
    rhs = RegOperand(cpu, insn, ahead, &carry_valid, &carry);
  }
  if (op != 13 && op != 15) {
    lhs = ReadReg(cpu, rn, ahead);
  }
  cin = (cpu->r[15] & ARM3_C) != 0 ? 1u : 0u;
  write_flags = s && (compare || rd != 15);
  restore = s && !compare && rd == 15;
  switch (op) {
    case 0:
      result = lhs & rhs;
      if (write_flags) {
        LogicFlags(cpu, result, carry_valid, carry);
      }
      break;
    case 1:
      result = lhs ^ rhs;
      if (write_flags) {
        LogicFlags(cpu, result, carry_valid, carry);
      }
      break;
    case 2:
      result = Sub(cpu, lhs, rhs, 1, write_flags);
      break;
    case 3:
      result = Sub(cpu, rhs, lhs, 1, write_flags);
      break;
    case 4:
      result = Add(cpu, lhs, rhs, 0, write_flags);
      break;
    case 5:
      result = Add(cpu, lhs, rhs, cin, write_flags);
      break;
    case 6:
      result = Sub(cpu, lhs, rhs, cin, write_flags);
      break;
    case 7:
      result = Sub(cpu, rhs, lhs, cin, write_flags);
      break;
    case 8:
      result = lhs & rhs;
      LogicFlags(cpu, result, carry_valid, carry);
      break;
    case 9:
      result = lhs ^ rhs;
      LogicFlags(cpu, result, carry_valid, carry);
      break;
    case 10:
      result = Sub(cpu, lhs, rhs, 1, true);
      break;
    case 11:
      result = Add(cpu, lhs, rhs, 0, true);
      break;
    case 12:
      result = lhs | rhs;
      if (write_flags) {
        LogicFlags(cpu, result, carry_valid, carry);
      }
      break;
    case 13:
      result = rhs;
      if (write_flags) {
        LogicFlags(cpu, result, carry_valid, carry);
      }
      break;
    case 14:
      result = lhs & ~rhs;
      if (write_flags) {
        LogicFlags(cpu, result, carry_valid, carry);
      }
      break;
    default:
      result = ~rhs;
      if (write_flags) {
        LogicFlags(cpu, result, carry_valid, carry);
      }
      break;
  }
  if (!compare) {
    WriteRd(cpu, rd, result, restore);
  }
  return cycles;
}

static int ExecuteMul(Arm3* cpu, uint32_t insn) {
  bool accumulate = (insn & (1u << 21)) != 0;
  bool s = (insn & (1u << 20)) != 0;
  int rd = (int)((insn >> 16) & 15u);
  int rn = (int)((insn >> 12) & 15u);
  int rs = (int)((insn >> 8) & 15u);
  int rm = (int)(insn & 15u);
  uint32_t result = (uint32_t)((uint64_t)ReadReg(cpu, rm, 8) * ReadReg(cpu, rs, 8));
  if (accumulate) {
    result += ReadReg(cpu, rn, 8);
  }
  if (rd != 15) {
    cpu->r[rd] = result;
    if (s) {
      SetNZ(cpu, result);
    }
  }
  return 4;
}

static int ExecuteSwap(Arm3* cpu, uint32_t insn) {
  bool byte = (insn & (1u << 22)) != 0;
  int rn = (int)((insn >> 16) & 15u);
  int rd = (int)((insn >> 12) & 15u);
  int rm = (int)(insn & 15u);
  uint32_t addr = ReadAddr(cpu, rn);
  uint32_t stored = ReadReg(cpu, rm, 8);
  uint32_t temp;
  if (byte) {
    temp = BusLoad8(cpu, addr);
    BusStore8(cpu, addr, (uint8_t)stored);
  } else {
    temp = BusLoad32(cpu, addr);
    BusStore32(cpu, addr, stored);
  }
  WriteRd(cpu, rd, temp, false);
  return 4;
}

static int ExecuteLoadStore(Arm3* cpu, uint32_t insn) {
  bool reg_off = (insn & (1u << 25)) != 0;
  bool pre = (insn & (1u << 24)) != 0;
  bool up = (insn & (1u << 23)) != 0;
  bool byte = (insn & (1u << 22)) != 0;
  bool writeback = !pre || (insn & (1u << 21)) != 0;
  bool load = (insn & (1u << 20)) != 0;
  int rn = (int)((insn >> 16) & 15u);
  int rd = (int)((insn >> 12) & 15u);
  uint32_t offset;
  uint32_t base;
  uint32_t modified;
  uint32_t addr;
  uint32_t value;
  bool carry_valid;
  bool carry;
  if (reg_off && (insn & (1u << 4)) != 0) {
    Take(cpu, ARM3_MODE_SVC, 0x04, cpu->pc + 4, true, false);
    return 4;
  }
  if (reg_off) {
    offset = RegOperand(cpu, insn, 8, &carry_valid, &carry);
  } else {
    offset = insn & 0xFFFu;
  }
  base = ReadAddr(cpu, rn);
  modified = up ? base + offset : base - offset;
  addr = pre ? modified : base;
  if (load) {
    if (byte) {
      value = BusLoad8(cpu, addr);
    } else {
      uint32_t shift = (addr & 3u) * 8u;
      value = BusLoad32(cpu, addr);
      if (shift != 0) {
        value = (value >> shift) | (value << (32u - shift));
      }
    }
    WriteRd(cpu, rd, value, false);
  } else if (byte) {
    BusStore8(cpu, addr, (uint8_t)ReadReg(cpu, rd, 8));
  } else {
    BusStore32(cpu, addr, ReadReg(cpu, rd, 8));
  }
  if (writeback && !(load && rd == rn)) {
    if (rn == 15) {
      WriteRd(cpu, 15, modified, false);
    } else {
      cpu->r[rn] = modified;
    }
  }
  return load ? 3 : 2;
}

static int ExecuteBlock(Arm3* cpu, uint32_t insn) {
  bool pre = (insn & (1u << 24)) != 0;
  bool up = (insn & (1u << 23)) != 0;
  bool s = (insn & (1u << 22)) != 0;
  bool w = (insn & (1u << 21)) != 0;
  bool load = (insn & (1u << 20)) != 0;
  int rn = (int)((insn >> 16) & 15u);
  uint32_t list = insn & 0xFFFFu;
  int regs[16];
  uint32_t stored[16];
  int count = 0;
  int i;
  uint32_t base;
  uint32_t cursor;
  uint32_t writeback;
  bool restore = load && s && (list & 0x8000u) != 0;
  bool user = s && !restore;
  for (i = 0; i < 16; i++) {
    if ((list & (1u << i)) != 0) {
      regs[count++] = i;
    }
  }
  base = ReadAddr(cpu, rn);
  writeback = up ? base + 4u * (uint32_t)count : base - 4u * (uint32_t)count;
  if (up) {
    cursor = pre ? base + 4u : base;
  } else {
    cursor = pre ? base - 4u * (uint32_t)count : base - 4u * (uint32_t)count + 4u;
  }
  if (!load) {
    for (i = 0; i < count; i++) {
      stored[i] = (user && regs[i] != 15) ? ReadUser(cpu, regs[i]) : ReadReg(cpu, regs[i], 8);
    }
    for (i = 0; i < count; i++) {
      BusStore32(cpu, cursor, stored[i]);
      cursor += 4u;
    }
  } else {
    for (i = 0; i < count; i++) {
      uint32_t value = BusLoad32(cpu, cursor);
      int reg = regs[i];
      cursor += 4u;
      if (reg == 15) {
        WriteRd(cpu, 15, value, restore);
      } else if (user) {
        WriteUser(cpu, reg, value);
      } else {
        cpu->r[reg] = value;
      }
    }
  }
  if (w && !(load && (list & (1u << rn)) != 0 && !user)) {
    if (rn == 15) {
      WriteRd(cpu, 15, writeback, false);
    } else {
      cpu->r[rn] = writeback;
    }
  }
  return 2 + count;
}

static int ExecuteBranch(Arm3* cpu, uint32_t insn) {
  int32_t offset = (int32_t)(insn << 8) >> 6;
  if ((insn & (1u << 24)) != 0) {
    cpu->r[14] = ((cpu->pc + 4u) & ARM3_PC_MASK) | (cpu->r[15] & 0xFC000003u);
  }
  cpu->pc = (cpu->pc + 8u + (uint32_t)offset) & ARM3_PC_MASK;
  cpu->branched = true;
  return 3;
}

static int Dispatch(Arm3* cpu, uint32_t insn) {
  if ((insn & 0x0E000090u) == 0x00000090u) {
    if ((insn & 0x0FC000F0u) == 0x00000090u) {
      return ExecuteMul(cpu, insn);
    }
    if ((insn & 0x0FB00FF0u) == 0x01000090u) {
      return ExecuteSwap(cpu, insn);
    }
    Take(cpu, ARM3_MODE_SVC, 0x04, cpu->pc + 4, true, false);
    return 4;
  }
  if ((insn & 0x0C000000u) == 0x00000000u) {
    return ExecuteData(cpu, insn);
  }
  if ((insn & 0x0C000000u) == 0x04000000u) {
    return ExecuteLoadStore(cpu, insn);
  }
  if ((insn & 0x0E000000u) == 0x08000000u) {
    return ExecuteBlock(cpu, insn);
  }
  if ((insn & 0x0E000000u) == 0x0A000000u) {
    return ExecuteBranch(cpu, insn);
  }
  if ((insn & 0x0F000000u) == 0x0F000000u) {
    Take(cpu, ARM3_MODE_SVC, 0x08, cpu->pc + 4, true, false);
    return 4;
  }
  Take(cpu, ARM3_MODE_SVC, 0x04, cpu->pc + 4, true, false);
  return 4;
}

Arm3* Arm3Create(void) {
  Arm3* cpu = calloc(1, sizeof(*cpu));
  if (cpu == NULL) {
    return NULL;
  }
  Arm3Reset(cpu);
  return cpu;
}

void Arm3Destroy(Arm3* cpu) {
  free(cpu);
}

void Arm3Reset(Arm3* cpu) {
  void* mem;
  uint32_t (*load32)(void*, uint32_t);
  void (*store32)(void*, uint32_t, uint32_t);
  uint8_t (*load8)(void*, uint32_t);
  void (*store8)(void*, uint32_t, uint8_t);
  bool (*irq)(void*);
  bool (*fiq)(void*);
  void (*fiq_ack)(void*);
  if (cpu == NULL) {
    return;
  }
  mem = cpu->mem;
  load32 = cpu->load32;
  store32 = cpu->store32;
  load8 = cpu->load8;
  store8 = cpu->store8;
  irq = cpu->irq;
  fiq = cpu->fiq;
  fiq_ack = cpu->fiq_ack;
  memset(cpu, 0, sizeof(*cpu));
  cpu->mem = mem;
  cpu->load32 = load32;
  cpu->store32 = store32;
  cpu->load8 = load8;
  cpu->store8 = store8;
  cpu->irq = irq;
  cpu->fiq = fiq;
  cpu->fiq_ack = fiq_ack;
  cpu->r[15] = ARM3_I | ARM3_F | ARM3_MODE_SVC;
}

void Arm3SetMemory(Arm3* cpu,
                   uint32_t (*load32)(void* ctx, uint32_t addr),
                   void (*store32)(void* ctx, uint32_t addr, uint32_t value),
                   uint8_t (*load8)(void* ctx, uint32_t addr),
                   void (*store8)(void* ctx, uint32_t addr, uint8_t value),
                   void* ctx) {
  if (cpu == NULL) {
    return;
  }
  cpu->load32 = load32;
  cpu->store32 = store32;
  cpu->load8 = load8;
  cpu->store8 = store8;
  cpu->mem = ctx;
}

void Arm3SetIrq(Arm3* cpu, bool (*irq)(void* ctx), bool (*fiq)(void* ctx), void (*fiq_ack)(void* ctx)) {
  if (cpu == NULL) {
    return;
  }
  cpu->irq = irq;
  cpu->fiq = fiq;
  cpu->fiq_ack = fiq_ack;
}

int Arm3Step(Arm3* cpu) {
  uint32_t insn;
  int cycles;
  if (cpu == NULL) {
    return 0;
  }
  if (cpu->fiq != NULL && (cpu->r[15] & ARM3_F) == 0 && cpu->fiq(cpu->mem)) {
    if (cpu->fiq_ack != NULL) {
      cpu->fiq_ack(cpu->mem);
    }
    Take(cpu, ARM3_MODE_FIQ, 0x1C, cpu->pc + 4, true, true);
    return 4;
  }
  if (cpu->irq != NULL && (cpu->r[15] & ARM3_I) == 0 && cpu->irq(cpu->mem)) {
    Take(cpu, ARM3_MODE_IRQ, 0x18, cpu->pc + 4, true, false);
    return 4;
  }
  cpu->branched = false;
  cpu->pc &= ARM3_PC_MASK;
  insn = BusLoad32(cpu, cpu->pc);
  if (!Condition(cpu, insn)) {
    cpu->pc = (cpu->pc + 4u) & ARM3_PC_MASK;
    return 1;
  }
  cycles = Dispatch(cpu, insn);
  if (!cpu->branched) {
    cpu->pc = (cpu->pc + 4u) & ARM3_PC_MASK;
  }
  return cycles < 1 ? 1 : cycles;
}

uint32_t Arm3Reg(const Arm3* cpu, int reg) {
  if (cpu == NULL || reg < 0 || reg > 15) {
    return 0;
  }
  if (reg == 15) {
    return (cpu->pc & ARM3_PC_MASK) | (cpu->r[15] & ~ARM3_PC_MASK);
  }
  return cpu->r[reg];
}

void Arm3SetReg(Arm3* cpu, int reg, uint32_t value) {
  if (cpu == NULL || reg < 0 || reg > 15) {
    return;
  }
  if (reg < 15) {
    cpu->r[reg] = value;
    return;
  }
  cpu->pc = value & ARM3_PC_MASK;
  ChangeMode(cpu, (int)(value & 3u));
  cpu->r[15] = (cpu->r[15] & ~0xFC000003u) | (value & 0xFC000003u);
}

uint32_t Arm3Pc(const Arm3* cpu) {
  if (cpu == NULL) {
    return 0;
  }
  return cpu->pc & ARM3_PC_MASK;
}
