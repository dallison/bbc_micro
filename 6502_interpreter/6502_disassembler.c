//
//  6502_disassembler.c
//  6502_interpreter
//
//  Created by David Allison on 5/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include "6502_disassembler.h"
#include "dstring.h"
#include <string.h>

static struct {
  const char* name;
  int value;
  int size;
  bool is_addr;
} regs[] = {
  {"$b0", 0x0, 1},
  {"$b1", 0x1, 1},
  {"$b2", 0x2, 1},
  {"$b3", 0x3, 1},
  {"$b4", 0x4, 1},
  {"$b5", 0x5, 1},
  {"$b6", 0x6, 1},
  {"$b7", 0x7, 1},
  {"$i0", 0x8, 2},
  {"$i1", 0xa, 2},
  {"$i2", 0xc, 2},
  {"$i3", 0xe, 2},
  {"$i4", 0x10, 2},
  {"$i5", 0x12, 2},
  {"$i6", 0x14, 2},
  {"$i7", 0x16, 2},
  {"$i8", 0x18, 2},
  {"$i9", 0x1a, 2},
  {"$i10", 0x1c, 2},
  {"$i11", 0x1e, 2},
  {"$i12", 0x20, 2},
  {"$i13", 0x22, 2},
  {"$i14", 0x24, 2},
  {"$i15", 0x26, 2},
  {"$l0", 0x28, 4},
  {"$l1", 0x2c, 4},
  {"$l2", 0x30, 4},
  {"$l3", 0x34, 4},
  {"$l4", 0x38, 4},
  {"$l5", 0x3c, 4},
  {"$l6", 0x40, 4},
  {"$l7", 0x44, 4},
  {"$x0", 0x48, 8},
  {"$x1", 0x50, 8},
  {"$x2", 0x58, 8},
  {"$x3", 0x60, 8},
  {"$f0", 0x68, 4},
  {"$f1", 0x6c, 4},
  {"$f2", 0x70, 4},
  {"$f3", 0x74, 4},
  {"$d0", 0x68, 4},
  {"$d1", 0x6c, 4},
  {"$d2", 0x70, 4},
  {"$d3", 0x74, 4},
  {"$fp", 0x7a, 2, true},
  {"$sp", 0x78, 2, true},
  {"$result", 0x7c, 2, true},
  {"$t0", 0x7e, 1},
  {"$t1", 0x7f, 1},
  {"$t2", 0x80, 1},
  {"$t3", 0x81, 1},
  {NULL, 0, 0},
};

bool NamedReg(const char* name, int* value, int* size, bool* is_addr) {
  for (int i = 0; regs[i].name != NULL; i++) {
    if (strcmp(regs[i].name, name) == 0) {
      *value = regs[i].value;
      *size = regs[i].size;
      *is_addr = regs[i].is_addr;
      return true;
    }
  }
  return false;
}

const char* RegNameOrAddress(int16_t value) {
  static char buf[32];
  for (int i = 0; regs[i].name != NULL; i++) {
     if (value >= regs[i].value && value < regs[i].value + regs[i].size) {
       if (value == regs[i].value) {
         return regs[i].name + 1;     // Remove $
       }
       snprintf(buf, sizeof(buf), "%s+%d", regs[i].name+1, value - regs[i].value);
       return buf;
      }
   }
  snprintf(buf, sizeof(buf), "0x%x", value & 0xffff);
  return buf;
}

const char* FindSymbolName(Loader* loader, uint16_t addr) {
  if (loader == NULL) {
    return NULL;
  }
  SymbolScope sym;
  if (LoaderFindSymbol(loader, addr, &sym)) {
    return sym.name;
  }
  return NULL;
}

static void* DisassembleGroup8(uint16_t addr, void* p, int inst, String* str) {
  static const char* mnemonics[16] = {
    "PHP",
    "CLC",
    "PLP",
    "SEC",
    "PHA",
    "CLI",
    "PLA",
    "SEI",
    "DEY",
    "TYA",
    "TAY",
    "CLV",
    "INY",
    "CLD",
    "INX",
    "SED",
  };
  StringPrintf(str,"%s\n", mnemonics[inst]);
  return (char*)p + 1;
}

static void* DisassembleALU(Loader* loader, uint16_t addr, void* p, int hi, int lo, String* str) {
  static const char* mnemonic[8] = {
    "ORA",
    "AND",
    "EOR",
    "ADC",
    "STA",
    "LDA",
    "CMP",
    "SBC",
  };
  
  if (hi == 8 && lo == 9) {
    StringPrintf(str,"BIT ");
  } else {
    StringPrintf(str,"%s ", mnemonic[hi / 2]);
  }
  int operand = 0;
  switch (lo) {
    case 1:
      // Zero page indexed
      operand = *(uint8_t*)p;
      if ((hi & 1) == 0) {
        StringPrintf(str, "(%s,X)\n", RegNameOrAddress(operand));
      } else {
        StringPrintf(str, "(%s),Y\n", RegNameOrAddress(operand));
      }
      return (char*)p + 1;
      
    case 5:
      // Zero page and indexed
      operand = *(uint8_t*)p;
     if ((hi & 1) == 0) {
        StringPrintf(str, "%s\n", RegNameOrAddress(operand));
      } else {
        StringPrintf(str, "0x%x,X\n", operand);
      }
      return (char*)p + 1;

    case 9:
      if ((hi & 1) == 0) {
        // Immediate and absolute indexed
        operand = *(uint8_t*)p;
        StringPrintf(str,"#0x%x\n", operand);
        return (char*)p + 1;
      } else {
        operand = *(uint16_t*)p;
        StringPrintf(str, "0x%x,Y\n", operand);
        return (char*)p + 2;
      }
    case 13:
      // Absolute and indexed X
      operand = *(uint16_t*)p;
      if ((hi & 1) == 0) {
        const char* sym = FindSymbolName(loader, operand);
        if (sym != NULL) {
          StringPrintf(str, "%s", sym);
        } else {
          StringPrintf(str, "%s", RegNameOrAddress(operand));
        }
      } else {
        StringPrintf(str, "0x%x,X", operand);
      }
      StringPrintf(str,"\n");
      return (char*)p + 2;
    default:
      return p;
  }
}

static void* DisassembleShiftAndMisc(uint16_t addr, void* p, int hi, int lo, String* str) {
  static const char* mnemonic[8] = {
    "ASL",
    "ROL",
    "LSR",
    "ROR",
    "STX",
    "LDX",
    "DEC",
    "INC",
  };
  
  // The second half of the hi value for lo == 10 are
  // different.
  static const char* special_mnemonic[8] = {
    "TXA",
    "TXS",
    "TAX",
    "TSX",
    "DEX",
    "PHX",
    "NOP",
    "PLX",
  };
  
  if (lo != 10) {
    StringPrintf(str,"%s ", mnemonic[hi / 2]);
  }
  
  int operand = 0;
  switch (lo) {
    case 6:
      // Zero page and indexed
      operand = *(uint8_t*)p;
      if ((hi & 1) == 0) {
        StringPrintf(str, "%s", RegNameOrAddress(operand));
      } else {
        StringPrintf(str, "0x%x,X", operand);
      }
      StringPrintf(str,"\n");
      return (char*)p + 1;

    case 10:
      // Accumulator and special
      if (hi < 8) {
        if ((hi & 1) == 0) {
          // Shifts and rotates.
          StringPrintf(str,"%s A", mnemonic[hi / 2]);
        } else {
          // Even more special.
          switch (hi) {
            case 1:
              StringPrintf(str,"INC A");
              break;
            case 3:
              StringPrintf(str,"DEC A");
              break;
            case 5:
              StringPrintf(str,"PHY");
              break;
            case 7:
              StringPrintf(str,"PLY");
              break;
          }
        }
      } else {
        StringPrintf(str,"%s", special_mnemonic[hi - 8]);
      }
      StringPrintf(str,"\n");
      return (char*)p ;
      
    case 14:
      // Absolute and indexed.
      operand = *(uint16_t*)p;
      if ((hi & 1) == 0) {
        StringPrintf(str, "%s", RegNameOrAddress(operand));
      } else {
        StringPrintf(str, "0x%x,X", operand);
      }
      StringPrintf(str,"\n");
      return (char*)p + 2;
    default:
      return p;
  }
}

static void* DisassembleGroup0(Loader* loader, uint16_t addr, void* p, int inst, String* str) {
  enum OpType {
    kNoOperand,
    kBranch,
    kImmediate,
    kAbsolute,
  };
  
  struct {
    const char* mnemonic;
    enum OpType type;
  } instructions[16] = {
    {"BRK", kNoOperand},
    {"BPL", kBranch},
    {"JSR", kAbsolute},
    {"BMI", kBranch},
    {"RTI", kNoOperand},
    {"BVC", kBranch},
    {"RTS", kNoOperand},
    {"BVS", kBranch},
    {"BRA", kBranch},
    {"BCC", kBranch},
    {"LDY", kImmediate},
    {"BCS", kBranch},
    {"CPY", kImmediate},
    {"BNE", kBranch},
    {"CPX", kImmediate},
    {"BEQ", kBranch},
  };
  StringPrintf(str,"%s", instructions[inst].mnemonic);
  uint16_t operand = 0;
  switch (instructions[inst].type) {
    case kBranch: {
      int16_t s_operand = *(int8_t*)p;
      StringPrintf(str," 0x%x\n", addr + s_operand + 2);
      return (char*)p + 1;
    }
    case kImmediate:
      operand = *(uint8_t*)p;
      StringPrintf(str," #0x%x\n", operand);
      return (char*)p + 1;
    case kNoOperand:
      StringPrintf(str, "\n");
      return p;
    case kAbsolute: {
      operand = *(uint16_t*)p;
      const char* sym = FindSymbolName(loader, operand & 0xffff);
      if (sym != NULL) {
        StringPrintf(str, " %s\n", sym);
      } else {
        StringPrintf(str, " %s\n", RegNameOrAddress(operand & 0xffff));
      }
      return (char*)p + 2;
    }
  }
}

static void* DisassembleGroup4(uint16_t addr, void* p, int hi, String* str) {
  static const char* mnemonic[16] = {
    "TSB",
    "TRB",
    "BIT",
    "BIT",
    "MVP",
    "MVN",
    "STZ",
    "STZ",
    "STY",
    "STY",
    "LDY",
    "LDY",
    "CPY",
    "PEI",
    "CPX",
    "PEA",
  };
  
  StringPrintf(str,"%s ", mnemonic[hi]);
  // All zero page, some indexed.
  int operand = *(uint8_t*)p;
  if (hi == 7 || hi == 9 || hi == 11) {
    StringPrintf(str, "0x%x,X", operand);
  } else {
    StringPrintf(str, "%s", RegNameOrAddress(operand));
  }
  StringPrintf(str,"\n");
  return (char*)p + 1;
}

static void* DisassembleGroup2(uint16_t addr, void* p, int hi, String* str) {
  static const char* mnemonic[16] = {
    "COP",
    "ORA",
    "JSL",
    "AND",
    "WDM",
    "EOR",
    "PER",
    "ADC",
    "BRL",
    "STA",
    "LDX",
    "LDA",
    "REP",
    "CMP",
    "SEP",
    "SBC",
  };
  StringPrintf(str,"%s ", mnemonic[hi]);
  int operand = *(uint16_t*)p;
  // Odd numbers are (zp) 65c02.
  if ((hi & 1) == 1) {
    // (zp) - byte.
    StringPrintf(str, "(%s)\n", RegNameOrAddress(*(uint8_t*)p));
  } else if (hi == 0xa) {
    // LDX #op
    StringPrintf(str,"#0x%x\n", operand & 0xff);
  } else {
    StringPrintf(str,"\n");    // Not on 65c02 or 6502.
  }
  return (char*)p + 1;
}

static void* DisassembleGroup12(Loader* loader, uint16_t addr, void* p, int hi, String* str) {
  static const char* mnemonic[16] = {
    "TSB",
    "TRB",
    "BIT",
    "BIT",
    "JMP",
    "JMP",
    "JMP",
    "JMP",
    "STY",
    "STZ",
    "LDY",
    "LDY",
    "CPY",
    "JMI",
    "CPX",
    "JSR",
  };
  
  StringPrintf(str,"%s ", mnemonic[hi]);
  // All absolute , some indexed, some indirect
  int operand = *(uint16_t*)p;
  if (hi == 6) {
    // JMP (a)
    StringPrintf(str, "(%s)", RegNameOrAddress(operand));
  } else if (hi == 7) {
    // JMP (a,X)
    StringPrintf(str, "(%s,X)", RegNameOrAddress(operand));
  } else if (hi == 3 || hi == 11) {
    StringPrintf(str, "0x%x,X", operand);
  } else {
    const char* sym = FindSymbolName(loader, operand);
    if (sym != NULL) {
      StringPrintf(str, "%s", sym);
    } else {
      StringPrintf(str, "%s", RegNameOrAddress(operand));
    }
  }
  StringPrintf(str,"\n");
  return (char*)p + 2;
}

void* Disassemble6502Instruction(Loader* loader, SymbolScope* current_symbol, uint16_t addr, void* p, FILE* fp) {
  const char* symbol_name = "???";
  int offset = 0;
  if (current_symbol != NULL) {
    symbol_name = current_symbol->name;
    offset = (int)(addr - current_symbol->start);
  }
  fprintf(fp, "%-20s+0x%04x: %04x  ", symbol_name, offset, addr);
  int lo = *(char*)p & 0xf;
  int hi = (*(char*)p & 0xf0) >> 4;
  void* operand = (char*)p + 1;
  String mnemonic = {0};
  void* after_inst;
  switch (lo) {
    case 0:
      // Branches mostly.
      after_inst = DisassembleGroup0(loader, addr, operand, hi, &mnemonic);
      break;
    case 1:
    case 5:
    case 9:
    case 13:
      // ALU.
      after_inst = DisassembleALU(loader, addr, operand, hi, lo, &mnemonic);
      break;
    case 2:
      after_inst = DisassembleGroup2(addr, operand, hi, &mnemonic);
      break;
    case 3:
    case 7:
    case 11:
    case 15:
    default:
      StringPrintf(&mnemonic,"???\n");
      // Not used in 6502.
      after_inst = operand;
      break;
    case 10:
    case 14:
    case 6:
      after_inst = DisassembleShiftAndMisc(addr, operand, hi, lo, &mnemonic);
      break;
    case 8:
      after_inst = DisassembleGroup8(addr, p, hi, &mnemonic);
      break;
    case 4:
      after_inst = DisassembleGroup4(addr, operand, hi, &mnemonic);
      break;
    case 12:
      after_inst = DisassembleGroup12(loader, addr, operand, hi, &mnemonic);
      break;
  }
  
  // Print the hex of the bytes making up the instruction.
  int count = 0;
  while (p < after_inst) {
    fprintf(fp, "%02x ", *(unsigned char*)p);
    count++;
    p++;
  }
  while (count < 3) {
    fprintf(fp, "   ");
    count++;
  }
  fprintf(fp, "  %s", mnemonic.value);
  StringDestruct(&mnemonic);
  return after_inst;
}
