//
//  6502_nmos.c
//  NMOS 6502 decimal arithmetic and the undocumented opcode set.
//

#include "6502_nmos.h"

#include "bbc_hardware.h"

static void SetNZ(W65C02Interpreter* cpu, uint8_t value) {
  cpu->flags.bits.z = value == 0;
  cpu->flags.bits.s = (value & 0x80) != 0;
}

static void AddBinary(W65C02Interpreter* cpu, uint8_t value) {
  uint8_t lhs = cpu->a;
  uint16_t sum = (uint16_t)lhs + value + cpu->flags.bits.c;
  cpu->flags.bits.c = sum > 255;
  cpu->flags.bits.v = ((~(lhs ^ value) & (lhs ^ (uint8_t)sum)) & 0x80) != 0;
  cpu->a = (uint8_t)sum;
  SetNZ(cpu, cpu->a);
}

static void SubBinary(W65C02Interpreter* cpu, uint8_t value) {
  uint8_t lhs = cpu->a;
  uint8_t borrow = (uint8_t)(cpu->flags.bits.c ^ 1);
  uint16_t diff = (uint16_t)lhs - value - borrow;
  cpu->flags.bits.c = diff <= 255;
  cpu->flags.bits.v = (((lhs ^ value) & (lhs ^ (uint8_t)diff)) & 0x80) != 0;
  cpu->a = (uint8_t)diff;
  SetNZ(cpu, cpu->a);
}

// NMOS decimal ADC sets N and Z from the binary add, not the BCD result.
static void AddNmosDecimal(W65C02Interpreter* cpu, uint8_t value) {
  uint8_t lhs = cpu->a;
  unsigned int bin = (unsigned int)lhs + value + cpu->flags.bits.c;
  unsigned int tmp = (lhs & 0x0f) + (value & 0x0f) + cpu->flags.bits.c;
  if (tmp > 9) {
    tmp += 6;
  }
  if (tmp <= 0x0f) {
    tmp = (tmp & 0x0f) + (lhs & 0xf0) + (value & 0xf0);
  } else {
    tmp = (tmp & 0x0f) + (lhs & 0xf0) + (value & 0xf0) + 0x10;
  }
  cpu->flags.bits.z = (bin & 0xff) == 0;
  cpu->flags.bits.s = (tmp & 0x80) != 0;
  cpu->flags.bits.v = ((lhs ^ tmp) & (value ^ tmp) & 0x80) != 0;
  if ((tmp & 0x1f0) > 0x90) {
    tmp += 0x60;
  }
  cpu->flags.bits.c = (tmp & 0xff0) > 0xf0;
  cpu->a = (uint8_t)tmp;
}

static void SubNmosDecimal(W65C02Interpreter* cpu, uint8_t value) {
  uint8_t lhs = cpu->a;
  unsigned int borrow = cpu->flags.bits.c ? 0 : 1;
  unsigned int diff = (unsigned int)lhs - value - borrow;
  unsigned int lo = (lhs & 0x0f) - (value & 0x0f) - borrow;
  int lo_borrow = (lo & 0x10) != 0;
  if (lo_borrow) {
    lo -= 6;
  }
  unsigned int hi = (lhs >> 4) - (value >> 4) - (unsigned int)lo_borrow;
  if (hi & 0x10) {
    hi -= 6;
  }
  cpu->flags.bits.z = (diff & 0xff) == 0;
  cpu->flags.bits.s = (diff & 0x80) != 0;
  cpu->flags.bits.v = ((lhs ^ diff) & (lhs ^ value) & 0x80) != 0;
  cpu->flags.bits.c = diff < 0x100;
  cpu->a = (uint8_t)((lo & 0x0f) | ((hi & 0x0f) << 4));
}

static void AddCmosDecimal(W65C02Interpreter* cpu, uint8_t value) {
  uint8_t lhs = cpu->a;
  unsigned int lo = (lhs & 0x0f) + (value & 0x0f) + cpu->flags.bits.c;
  if (lo >= 10) {
    lo += 6;
  }
  unsigned int hi = (lhs >> 4) + (value >> 4) + (lo >> 4);
  if (hi >= 10) {
    hi += 6;
  }
  cpu->a = (uint8_t)(((hi & 0x0f) << 4) | (lo & 0x0f));
  cpu->flags.bits.c = hi >= 16;
  cpu->flags.bits.v = ((~(lhs ^ value) & (lhs ^ cpu->a)) & 0x80) != 0;
  SetNZ(cpu, cpu->a);
}

static void SubCmosDecimal(W65C02Interpreter* cpu, uint8_t value) {
  uint8_t lhs = cpu->a;
  unsigned int borrow = cpu->flags.bits.c ? 0 : 1;
  unsigned int lo = (lhs & 0x0f) - (value & 0x0f) - borrow;
  int lo_borrow = (lo & 0x10) != 0;
  if (lo_borrow) {
    lo -= 6;
  }
  unsigned int hi = (lhs >> 4) - (value >> 4) - (unsigned int)lo_borrow;
  int hi_borrow = (hi & 0x10) != 0;
  if (hi_borrow) {
    hi -= 6;
  }
  cpu->a = (uint8_t)((lo & 0x0f) | ((hi & 0x0f) << 4));
  cpu->flags.bits.c = !hi_borrow;
  cpu->flags.bits.v = ((lhs ^ cpu->a) & (lhs ^ value) & 0x80) != 0;
  SetNZ(cpu, cpu->a);
}

void CpuAdd(W65C02Interpreter* cpu, uint8_t value) {
  if (!cpu->flags.bits.d) {
    AddBinary(cpu, value);
  } else if (cpu->nmos) {
    AddNmosDecimal(cpu, value);
  } else {
    AddCmosDecimal(cpu, value);
  }
}

void CpuSub(W65C02Interpreter* cpu, uint8_t value) {
  if (!cpu->flags.bits.d) {
    SubBinary(cpu, value);
  } else if (cpu->nmos) {
    SubNmosDecimal(cpu, value);
  } else {
    SubCmosDecimal(cpu, value);
  }
}

static uint8_t Rd(W65C02Interpreter* cpu, uint16_t addr) {
  if (cpu->bbc != NULL && addr >= 0xfc00 && addr <= 0xfeff) {
    W65C02CatchUp(cpu);
    cpu->extra_cycles++;
    W65C02NoteRead(cpu, addr);
    return BbcMachineRead(cpu->bbc, addr);
  }
  return cpu->memory[addr];
}

static void Wr(W65C02Interpreter* cpu, uint16_t addr, uint8_t value) {
  if (cpu->bbc != NULL && addr >= 0xfc00 && addr <= 0xfeff) {
    W65C02CatchUp(cpu);
    cpu->extra_cycles++;
    BbcMachineWrite(cpu->bbc, addr, value);
    W65C02NoteStore(cpu, addr);
    return;
  }
  cpu->memory[addr] = value;
  W65C02NoteStore(cpu, addr);
}

static uint8_t Asl(W65C02Interpreter* cpu, uint8_t value) {
  cpu->flags.bits.c = (value & 0x80) != 0;
  value = (uint8_t)(value << 1);
  SetNZ(cpu, value);
  return value;
}

static uint8_t Lsr(W65C02Interpreter* cpu, uint8_t value) {
  cpu->flags.bits.c = value & 1;
  value = (uint8_t)(value >> 1);
  SetNZ(cpu, value);
  return value;
}

static uint8_t Rol(W65C02Interpreter* cpu, uint8_t value) {
  uint8_t carry = cpu->flags.bits.c;
  cpu->flags.bits.c = (value & 0x80) != 0;
  value = (uint8_t)((value << 1) | carry);
  SetNZ(cpu, value);
  return value;
}

static uint8_t Ror(W65C02Interpreter* cpu, uint8_t value) {
  uint8_t carry = cpu->flags.bits.c;
  cpu->flags.bits.c = value & 1;
  value = (uint8_t)((value >> 1) | (carry << 7));
  SetNZ(cpu, value);
  return value;
}

static void CmpA(W65C02Interpreter* cpu, uint8_t value) {
  uint16_t diff = (uint16_t)cpu->a - value;
  cpu->flags.bits.c = cpu->a >= value;
  SetNZ(cpu, (uint8_t)diff);
}

static uint16_t ZpIndirect(const W65C02Interpreter* cpu, uint8_t zp) {
  uint8_t hi = (uint8_t)(zp + 1);
  return (uint16_t)(cpu->memory[zp] | (cpu->memory[hi] << 8));
}

static int Crossed(uint16_t base, uint8_t index) {
  return ((base & 0xff) + index) > 0xff;
}

static uint16_t OperandAddress(W65C02Interpreter* cpu, int column, int* cycles) {
  uint16_t pc = cpu->pc;
  uint8_t b1 = cpu->memory[(uint16_t)(pc + 1)];
  uint8_t b2 = cpu->memory[(uint16_t)(pc + 2)];
  uint16_t base;
  uint8_t zp;
  switch (column) {
    case 0x03:
      zp = (uint8_t)(b1 + cpu->x);
      *cycles = 8;
      cpu->pc = (uint16_t)(pc + 2);
      return ZpIndirect(cpu, zp);
    case 0x07:
      *cycles = 5;
      cpu->pc = (uint16_t)(pc + 2);
      return b1;
    case 0x0f:
      *cycles = 6;
      cpu->pc = (uint16_t)(pc + 3);
      return (uint16_t)(b1 | (b2 << 8));
    case 0x13:
      base = ZpIndirect(cpu, b1);
      *cycles = 8;
      cpu->pc = (uint16_t)(pc + 2);
      return (uint16_t)(base + cpu->y);
    case 0x17:
      *cycles = 6;
      cpu->pc = (uint16_t)(pc + 2);
      return (uint8_t)(b1 + cpu->x);
    case 0x1b:
      base = (uint16_t)(b1 | (b2 << 8));
      *cycles = 7;
      cpu->pc = (uint16_t)(pc + 3);
      return (uint16_t)(base + cpu->y);
    case 0x1f:
      base = (uint16_t)(b1 | (b2 << 8));
      *cycles = 7;
      cpu->pc = (uint16_t)(pc + 3);
      return (uint16_t)(base + cpu->x);
    default:
      return 0;
  }
}

static int RmwIllegal(W65C02Interpreter* cpu, uint8_t opcode) {
  int column = opcode & 0x1f;
  int family = opcode >> 5;
  int cycles = 0;
  uint16_t addr;
  uint8_t value;
  if (column != 0x03 && column != 0x07 && column != 0x0f && column != 0x13 &&
      column != 0x17 && column != 0x1b && column != 0x1f) {
    return -1;
  }
  if (family != 0 && family != 1 && family != 2 && family != 3 && family != 6 &&
      family != 7) {
    return -1;
  }
  addr = OperandAddress(cpu, column, &cycles);
  value = Rd(cpu, addr);
  Wr(cpu, addr, value);
  if (family == 0) {
    value = Asl(cpu, value);
    cpu->a = (uint8_t)(cpu->a | value);
    SetNZ(cpu, cpu->a);
  } else if (family == 1) {
    value = Rol(cpu, value);
    cpu->a = (uint8_t)(cpu->a & value);
    SetNZ(cpu, cpu->a);
  } else if (family == 2) {
    value = Lsr(cpu, value);
    cpu->a = (uint8_t)(cpu->a ^ value);
    SetNZ(cpu, cpu->a);
  } else if (family == 3) {
    value = Ror(cpu, value);
    CpuAdd(cpu, value);
  } else if (family == 6) {
    value = (uint8_t)(value - 1);
    CmpA(cpu, value);
  } else {
    value = (uint8_t)(value + 1);
    CpuSub(cpu, value);
  }
  Wr(cpu, addr, value);
  return cycles;
}

static void NopBytes(W65C02Interpreter* cpu, int bytes) {
  cpu->pc = (uint16_t)(cpu->pc + bytes);
}

static int NopIndexed(W65C02Interpreter* cpu, uint8_t index) {
  uint16_t pc = cpu->pc;
  uint16_t base = (uint16_t)(cpu->memory[(uint16_t)(pc + 1)] |
                             (cpu->memory[(uint16_t)(pc + 2)] << 8));
  cpu->pc = (uint16_t)(pc + 3);
  return 4 + Crossed(base, index);
}

static int StoreHigh(W65C02Interpreter* cpu, uint8_t index, uint8_t value, int use_x) {
  uint16_t pc = cpu->pc;
  uint8_t lo = cpu->memory[(uint16_t)(pc + 1)];
  uint8_t hi = cpu->memory[(uint16_t)(pc + 2)];
  uint16_t base = (uint16_t)(lo | (hi << 8));
  uint16_t addr = (uint16_t)(base + index);
  uint8_t mask = (uint8_t)(hi + 1);
  if (Crossed(base, index)) {
    addr = (uint16_t)((addr & 0xff) | ((value & mask) << 8));
  }
  if (use_x) {
    value = (uint8_t)(value & mask);
  }
  Wr(cpu, addr, (uint8_t)(value & mask));
  cpu->pc = (uint16_t)(pc + 3);
  return 5;
}

int NmosExecute(W65C02Interpreter* cpu, uint8_t opcode) {
  uint16_t pc = cpu->pc;
  uint8_t b1 = cpu->memory[(uint16_t)(pc + 1)];
  uint8_t imm;
  uint16_t addr;
  uint8_t value;
  int rmw;
  switch (opcode) {
    case 0x02:
    case 0x12:
    case 0x22:
    case 0x32:
    case 0x42:
    case 0x52:
    case 0x62:
    case 0x72:
    case 0x92:
    case 0xb2:
    case 0xd2:
    case 0xf2:
      cpu->jammed = true;
      return 1;
    case 0x80:
    case 0x82:
    case 0x89:
    case 0xc2:
    case 0xe2:
      NopBytes(cpu, 2);
      return 2;
    case 0x04:
    case 0x44:
    case 0x64:
      NopBytes(cpu, 2);
      return 3;
    case 0x14:
    case 0x34:
    case 0x54:
    case 0x74:
    case 0xd4:
    case 0xf4:
      NopBytes(cpu, 2);
      return 4;
    case 0x0c:
      NopBytes(cpu, 3);
      return 4;
    case 0x1c:
    case 0x3c:
    case 0x5c:
    case 0x7c:
    case 0xdc:
    case 0xfc:
      return NopIndexed(cpu, cpu->x);
    case 0x1a:
    case 0x3a:
    case 0x5a:
    case 0x7a:
    case 0xda:
    case 0xfa:
      NopBytes(cpu, 1);
      return 2;
    case 0x0b:
    case 0x2b:
      cpu->a = (uint8_t)(cpu->a & b1);
      SetNZ(cpu, cpu->a);
      cpu->flags.bits.c = cpu->flags.bits.s;
      cpu->pc = (uint16_t)(pc + 2);
      return 2;
    case 0x4b:
      cpu->a = (uint8_t)(cpu->a & b1);
      cpu->flags.bits.c = cpu->a & 1;
      cpu->a = (uint8_t)(cpu->a >> 1);
      SetNZ(cpu, cpu->a);
      cpu->pc = (uint16_t)(pc + 2);
      return 2;
    case 0x6b:
      cpu->a = (uint8_t)(cpu->a & b1);
      value = (uint8_t)((cpu->a >> 1) | (cpu->flags.bits.c << 7));
      cpu->flags.bits.c = (value & 0x40) != 0;
      cpu->flags.bits.v = ((value ^ (uint8_t)(value << 1)) & 0x40) != 0;
      cpu->a = value;
      SetNZ(cpu, cpu->a);
      cpu->pc = (uint16_t)(pc + 2);
      return 2;
    case 0xab:
      cpu->a = (uint8_t)((cpu->a | 0xee) & b1);
      cpu->x = cpu->a;
      SetNZ(cpu, cpu->a);
      cpu->pc = (uint16_t)(pc + 2);
      return 2;
    case 0xcb:
      imm = (uint8_t)(cpu->a & cpu->x);
      cpu->flags.bits.c = imm >= b1;
      cpu->x = (uint8_t)(imm - b1);
      SetNZ(cpu, cpu->x);
      cpu->pc = (uint16_t)(pc + 2);
      return 2;
    case 0xeb:
      CpuSub(cpu, b1);
      cpu->pc = (uint16_t)(pc + 2);
      return 2;
    case 0x83:
      addr = ZpIndirect(cpu, (uint8_t)(b1 + cpu->x));
      Wr(cpu, addr, (uint8_t)(cpu->a & cpu->x));
      cpu->pc = (uint16_t)(pc + 2);
      return 6;
    case 0x87:
      Wr(cpu, b1, (uint8_t)(cpu->a & cpu->x));
      cpu->pc = (uint16_t)(pc + 2);
      return 3;
    case 0x8f:
      addr = (uint16_t)(b1 | (cpu->memory[(uint16_t)(pc + 2)] << 8));
      Wr(cpu, addr, (uint8_t)(cpu->a & cpu->x));
      cpu->pc = (uint16_t)(pc + 3);
      return 4;
    case 0x97:
      Wr(cpu, (uint8_t)(b1 + cpu->y), (uint8_t)(cpu->a & cpu->x));
      cpu->pc = (uint16_t)(pc + 2);
      return 4;
    case 0xa3:
    case 0xa7:
    case 0xaf:
    case 0xb3:
    case 0xb7:
    case 0xbf: {
      int cycles = 4;
      int cross = 0;
      if (opcode == 0xa3) {
        addr = ZpIndirect(cpu, (uint8_t)(b1 + cpu->x));
        cycles = 6;
        cpu->pc = (uint16_t)(pc + 2);
      } else if (opcode == 0xa7) {
        addr = b1;
        cycles = 3;
        cpu->pc = (uint16_t)(pc + 2);
      } else if (opcode == 0xaf) {
        addr = (uint16_t)(b1 | (cpu->memory[(uint16_t)(pc + 2)] << 8));
        cycles = 4;
        cpu->pc = (uint16_t)(pc + 3);
      } else if (opcode == 0xb3) {
        uint16_t base = ZpIndirect(cpu, b1);
        cross = Crossed(base, cpu->y);
        addr = (uint16_t)(base + cpu->y);
        cycles = 5 + cross;
        cpu->pc = (uint16_t)(pc + 2);
      } else if (opcode == 0xb7) {
        addr = (uint8_t)(b1 + cpu->y);
        cycles = 4;
        cpu->pc = (uint16_t)(pc + 2);
      } else {
        uint16_t base = (uint16_t)(b1 | (cpu->memory[(uint16_t)(pc + 2)] << 8));
        cross = Crossed(base, cpu->y);
        addr = (uint16_t)(base + cpu->y);
        cycles = 4 + cross;
        cpu->pc = (uint16_t)(pc + 3);
      }
      value = Rd(cpu, addr);
      cpu->a = value;
      cpu->x = value;
      SetNZ(cpu, value);
      return cycles;
    }
    case 0x93: {
      uint16_t base = ZpIndirect(cpu, b1);
      uint8_t hi = cpu->memory[(uint8_t)(b1 + 1)];
      addr = (uint16_t)(base + cpu->y);
      value = (uint8_t)(cpu->a & cpu->x & (uint8_t)(hi + 1));
      if (Crossed(base, cpu->y)) {
        addr = (uint16_t)((addr & 0xff) | (value << 8));
      }
      Wr(cpu, addr, value);
      cpu->pc = (uint16_t)(pc + 2);
      return 6;
    }
    case 0x9b:
      cpu->s = (uint8_t)(cpu->a & cpu->x);
      return StoreHigh(cpu, cpu->y, cpu->s, 1);
    case 0x9c:
      return StoreHigh(cpu, cpu->x, cpu->y, 1);
    case 0x9e:
      return StoreHigh(cpu, cpu->y, cpu->x, 1);
    case 0x9f:
      return StoreHigh(cpu, cpu->y, (uint8_t)(cpu->a & cpu->x), 1);
    case 0xbb: {
      uint16_t base = (uint16_t)(b1 | (cpu->memory[(uint16_t)(pc + 2)] << 8));
      int cross = Crossed(base, cpu->y);
      value = (uint8_t)(Rd(cpu, (uint16_t)(base + cpu->y)) & cpu->s);
      cpu->a = value;
      cpu->x = value;
      cpu->s = value;
      SetNZ(cpu, value);
      cpu->pc = (uint16_t)(pc + 3);
      return 4 + cross;
    }
    default:
      break;
  }
  rmw = RmwIllegal(cpu, opcode);
  return rmw;
}
