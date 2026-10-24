//
//  6502_disassembler.c
//  6502_interpreter
//
//  Created by David Allison on 5/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include "6502_disassembler.h"
#include "dstring.h"
#include <stdio.h>

static const char* HexAddress(int value) {
  static char buf[32];
  snprintf(buf, sizeof(buf), "0x%x", value & 0xffff);
  return buf;
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

static void* DisassembleALU(void* p, int hi, int lo, String* str) {
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
        StringPrintf(str, "(%s,X)\n", HexAddress(operand));
      } else {
        StringPrintf(str, "(%s),Y\n", HexAddress(operand));
      }
      return (char*)p + 1;
      
    case 5:
      // Zero page and indexed
      operand = *(uint8_t*)p;
     if ((hi & 1) == 0) {
        StringPrintf(str, "%s\n", HexAddress(operand));
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
        StringPrintf(str, "%s", HexAddress(operand));
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
        StringPrintf(str, "%s", HexAddress(operand));
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
        StringPrintf(str, "%s", HexAddress(operand));
      } else {
        StringPrintf(str, "0x%x,X", operand);
      }
      StringPrintf(str,"\n");
      return (char*)p + 2;
    default:
      return p;
  }
}

static void* DisassembleGroup0(uint16_t addr, void* p, int inst, String* str) {
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
      StringPrintf(str, " %s\n", HexAddress(operand));
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
    StringPrintf(str, "%s", HexAddress(operand));
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
    StringPrintf(str, "(%s)\n", HexAddress(*(uint8_t*)p));
  } else if (hi == 0xa) {
    // LDX #op
    StringPrintf(str,"#0x%x\n", operand & 0xff);
  } else {
    StringPrintf(str,"\n");    // Not on 65c02 or 6502.
  }
  return (char*)p + 1;
}

static void* DisassembleGroup12(void* p, int hi, String* str) {
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
    StringPrintf(str, "(%s)", HexAddress(operand));
  } else if (hi == 7) {
    // JMP (a,X)
    StringPrintf(str, "(%s,X)", HexAddress(operand));
  } else if (hi == 3 || hi == 11) {
    StringPrintf(str, "0x%x,X", operand);
  } else {
    StringPrintf(str, "%s", HexAddress(operand));
  }
  StringPrintf(str,"\n");
  return (char*)p + 2;
}

void* Disassemble6502Instruction(uint16_t addr, void* p, FILE* fp) {
  fprintf(fp, "%04x  ", addr);
  int lo = *(char*)p & 0xf;
  int hi = (*(char*)p & 0xf0) >> 4;
  void* operand = (char*)p + 1;
  String mnemonic = {0};
  void* after_inst;
  switch (lo) {
    case 0:
      // Branches mostly.
      after_inst = DisassembleGroup0(addr, operand, hi, &mnemonic);
      break;
    case 1:
    case 5:
    case 9:
    case 13:
      // ALU.
      after_inst = DisassembleALU(operand, hi, lo, &mnemonic);
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
      after_inst = DisassembleGroup12(operand, hi, &mnemonic);
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
