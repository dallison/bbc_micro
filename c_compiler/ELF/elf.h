//
//  elf.h
//  c_compiler
//
//  Created by David Allison on 1/2/18.
//  Copyright © 2018 David Allison. All rights reserved.
//

#ifndef elf_h
#define elf_h

#include <stdio.h>
#include <stdint.h>
#include "vector.h"
#include "dstring.h"
#include "buffer.h"

// Structures and definitions for ELF64 files.  This must
// match the ELF specification.

// NOTES:
// 1. The symbol table section (.symtab) needs special values in its
//    'link' and 'info' fields.  The link field is the index of the string
//     table section (.strtab).  The info field is one greater than the
//     index of the last local symbol.
// 2. The symtab section has special ordering requirements.  The order must be:
//    a. File symbol.
//    b. Section symbols.
//    c. Local symbols.
//    d. Global or undefined symbols.
// 3. The symtab section must have an empty (all zeroes) entry at index 0.
// 4. The relocation sections are named for the section upon which their
//    relocations are applied.  The name is formed by adding the prefix ".rela"
//    (or .rel) to their target section name.  For example, the relocation
//    table for the .text section is called .rela.text.
// 5. The relocation sections also must refer to the section that they
//    relocate, exactly as in note 1 above.
// 6. The .symtab and relocation sections have fixed size entries so they must
//    have their 'entsize' field set to the size of their entries.
// 7. Section names are held in their own string table.  This is usually called
//    .shstrtab and its index must be set in the file header's 'shstrndx' field.

// ELF 64 types.
typedef uint64_t ELF_Addr;
typedef uint16_t ELF_Half;
typedef uint16_t ELF_SHalf;
typedef uint64_t ELF_Off;
typedef uint32_t ELF_Sword;
typedef uint32_t ELF_Word;
typedef uint64_t ELF_Xword;
typedef uint64_t ELF_Sxword;

// Type of the ELF file (in the 'type' field of the file header).
#define ET(t) kELFType_##t
typedef enum {
  ET(none),   // No type specified.
  ET(rel),    // File is relocatable (an object file, typically).
  ET(exec),   // File is executable (a binary).
  ET(dyn),    // File is a dynamic executable (shared object).
  ET(core),   // File is a core dump file.
} ELFType;

// ELF Segment type.
#define PT(x) kELFSegmentType_##x
typedef enum {
 PT(null),
 PT(load),        // Loadable segment.
 PT(dynamic),     // DYNAMIC segment (for .so files)
 PT(interp),      // Program interpreter.
 PT(note),        // General note.
 PT(shlib),       // Shared library.
 PT(phdr),        // Program header
 PT(tls),         // Thread local storage.
} ELFSegmentType;

#define PF(x) kELFSegmentFlags_##x
typedef enum {
  PF(x) = 1 << 0,   // Executable.
  PF(w) = 1 << 1,   // Writeable.
  PF(r) = 1 << 2,   // Readable.
} ELFSegmentFlags;

// Symbol binding values.
#define STB(x) kELFSymbolType_##x
typedef enum {
  STB(local),       // Local symbol (seen only in this file)
  STB(global),      // Global symbol (seen by other files).
  STB(weak),        // Weak symbol (might not exist).
} ELFSymbolBinding;

// Symbol type values.  Each symbol has a type.
#define STT(x) kELFSymbolType_##x
typedef enum {
  STT(notype),     // No type specified.
  STT(object),     // Symbol is an object (a variable).
  STT(func),       // A function (always take address).
  STT(section),    // A section name symbol.
  STT(file),       // A file name symbol.
  STT(common),     // A common symbol (might be merged with another symbol).
  STT(tls),        // Thread local storage.
} ELFSymbolType;

// For a symbol, the binding is the top 4 bits and
// the type is the lower 4 bits of the info field.
#define ELF_ST_BIND(x) ((x) >> 4)
#define ELF_ST_TYPE(x) (((uint32_t)x) & 0xf)

// An ELF symbol struct, appears exactly in this form in the symbol table.
typedef struct {
  ELF_Word name;    // Symbol name, index into string table.
  uint8_t info;       // Type and binding.
  uint8_t other;      // Not used (must be zero).
  ELF_Half shndx;    // Section index.
  ELF_Addr value;
  ELF_Xword size;    // Size in bytes.
} ELFSymbol;

// A relocation.  Used to apply a symbol's value to an offset within
// a section at load time.  The info field contains the symbol index
// and the relocation type.  The relocation type specifies how the symbol's
// value is to used to modify the memory in the target section.
typedef struct  {
  ELF_Addr offset;     // Location to apply the action.
  ELF_Xword info;      // Encoded index and type.
  ELF_Sxword addend;   // Constant used to compute value.
} ELFRelocation;

// The 'info' field of a relocation is encoded as two
// 32 bit words:
// +-------------------------------+-----------------------------+
// |     symbol index              |        relocation type      |
// +-------------------------------+-----------------------------+

#define ELF_R_SYM(info)             ((info)>>32)
#define ELF_R_TYPE(info)            ((ELF_Word)(info))
#define ELF_R_INFO(sym, type)       (((ELF_Xword)(sym)<<32)+(ELF_Xword)(type))

// Relocation types we support.
// P-CODE
#define R_PCODE_CALL 1        // Call direct to symbol.
#define R_PCODE_ABS 2         // Move symbol address to reg.
#define R_PCODE_JMP 3         // Jump to symbol.
#define R_PCODE_DATA64 4      // 64-bit data.
#define R_PCODE_DATA32 5      // 32-bit data.
#define R_PCODE_ADD16 6       // Add 16-bit.
#define R_PCODE_ADD32 7       // Add 32-bit.
#define R_PCODE_ADD64 8       // Add 64-bit.
#define R_PCODE_SUB16 9       // Subtract 16-bit.
#define R_PCODE_SUB32 10      // Subtract 32-bit.
#define R_PCODE_SUB64 11      // Subtract 64-bit.
#define R_PCODE_CALL_PLT 12   // Call via PLT.
#define R_PCODE_GOT_ENTRY 13  // Address of GOT entry for data.
#define R_PCODE_GOT_DATA 14       // Value of data in GOT.
#define R_PCODE_GOT_FUNC 15       // Value of function in GOT.
#define R_PCODE_PCREL 16        // PC relative address.
#define R_PCODE_GOT_TLS_IE 17    // TLS IE GOT entry.
#define R_PCODE_GOT_TLS_GD 18    // TLS GD GOT entry.
#define R_PCODE_TLS_TP_OFF 19    // TLS Thread pointer offset.
#define R_PCODE_GOT_TLS_OFFSET 20
#define R_PCODE_GOT_TLS_MODID 21
#define R_PCODE_RELATIVE 22

// RISC-V
#define R_RISCV_NONE 0        // No action.
#define R_RISCV_32 1          // Add 32 bit symbol value.
#define R_RISCV_64 2          // Add 64 bit symbol value.
#define R_RISCV_RELATIVE 3    // Add load address of shared object.
#define R_RISCV_COPY 4        // Copy data from shared object.
#define R_RISCV_JUMP_SLOT 5   // Set GOT entry.
#define R_RISCV_TLS_DTPMOD32 6  // TLS DTV module ID
#define R_RISCV_TLS_DTPMOD64 7  // TLS DTV module ID
#define R_RISCV_TLS_DTPREL32 8  // TLS
#define R_RISCV_TLS_DTPREL64 9
#define R_RISCV_TLS_TPREL32 10
#define R_RISCV_TLS_TPREL64 11
#define R_RISCV_BRANCH 16     // PC relative branch.
#define R_RISCV_JAL 17        // PC relative jump.
#define R_RISCV_CALL 18       // PC relative call.
#define R_RISCV_CALL_PLT 19   // PC relative call via PLT.
#define R_RISCV_GOT_HI20 20   // PC relative GOT high 20 bits.
#define R_RISCV_TLS_GOT_HI20 21 // TLS IE high 20 bits.
#define R_RISCV_TLS_GD_HI20 22  // TLS GD high 20.
#define R_RISCV_PCREL_HI20 23   // PC relative high 20 bits.
#define R_RISCV_PCREL_LO12_I 24 // PC relative low 12 bits (I-type)
#define R_RISCV_PCREL_LO12_S 25 // PC relaitve low 12 bits (S-type)
#define R_RISCV_HI20 26         // Absolute high 20 bits.
#define R_RISCV_LO12_I 27       // Absolute low 12 bits (I-type)
#define R_RISCV_LO12_S 28       // Absolute low 12 bits (S-type)
#define R_RISCV_TPREL_HI20 29
#define R_RISCV_TPREL_LO12_I 30
#define R_RISCV_TPREL_LO12_S 31
#define R_RISCV_TPREL_ADD 32
#define R_RISCV_ADD8 33         // Add 8 bits.
#define R_RISCV_ADD16 34        // Add 16 bits.
#define R_RISCV_ADD32 35        // Add 32 bits.
#define R_RISCV_ADD64 36        // Add 64 bits.
#define R_RISCV_SUB8 37         // Subtract 8 bits.
#define R_RISCV_SUB16 38        // Subtract 16 bits.
#define R_RISCV_SUB32 39        // Subtract 32 bits.
#define R_RISCV_SUB64 40        // Subtract 64 bits.
#define R_RISCV_ALIGN 43        // Align
#define R_RISCV_RVC_BRANCH 44   // Branch.
#define R_RISCV_RVC_JUMP 45     // Jump.
#define R_RISCV_RVC_LUI 46      // Address.
#define R_RISCV_TPREL_I 49
#define R_RISCV_TPREL_S 50
#define R_RISCV_RELAX 51        // Relax instruction pair.

// Western Design Center 65C02 processor.
#define R_W65C02_JSR 1         // Call direct to symbol.
#define R_W65C02_JMP 2         // Move symbol address to reg.
#define R_W65C02_DATA16 3      // 16-bit data.
#define R_W65C02_DATA32 4      // 32-bit data.
#define R_W65C02_DATA64 5      // 64-bit data.
#define R_W65C02_BYTE0 6
#define R_W65C02_BYTE1 7
#define R_W65C02_BYTE2 8
#define R_W65C02_BYTE3 9
#define R_W65C02_BYTE4 10
#define R_W65C02_BYTE5 11
#define R_W65C02_BYTE6 12
#define R_W65C02_BYTE7 13
#define R_W65C02_ADD8 14         // Add 8 bits.
#define R_W65C02_ADD16 15        // Add 16 bits.
#define R_W65C02_ADD32 16        // Add 32 bits.
#define R_W65C02_ADD64 17        // Add 64 bits.
#define R_W65C02_SUB8 18         // Subtract 8 bits.
#define R_W65C02_SUB16 19        // Subtract 16 bits.
#define R_W65C02_SUB32 20        // Subtract 32 bits.
#define R_W65C02_SUB64 21        // Subtract 64 bits.

// AARCH64v8 (AARCH64) relocation types.
#define R_AARCH64_NONE            0  // No relocation.

//  ILP32
#define R_AARCH64_P32_ABS32      1  // Direct 32 bit.  
#define R_AARCH64_P32_COPY    180  // Copy symbol at runtime.  
#define R_AARCH64_P32_GLOB_DAT    181  // Create GOT entry.  
#define R_AARCH64_P32_JUMP_SLOT    182  // Create PLT entry.  
#define R_AARCH64_P32_RELATIVE    183  // Adjust by program base.  
#define R_AARCH64_P32_TLS_DTPMOD  184  // Module number, 32 bit.  
#define R_AARCH64_P32_TLS_DTPREL  185  // Module-relative offset, 32 bit.  
#define R_AARCH64_P32_TLS_TPREL    186  // TP-relative offset, 32 bit.  
#define R_AARCH64_P32_TLSDESC    187  // TLS Descriptor.  
#define R_AARCH64_P32_IRELATIVE    188  // STT_GNU_IFUNC relocation. 

// LP64 AArch64
#define R_AARCH64_ABS64         257  // Direct 64 bit. 
#define R_AARCH64_ABS32         258  // Direct 32 bit.  
#define R_AARCH64_ABS16    259  // Direct 16-bit.  
#define R_AARCH64_PREL64  260  // PC-relative 64-bit.  
#define R_AARCH64_PREL32  261  // PC-relative 32-bit.  
#define R_AARCH64_PREL16  262  // PC-relative 16-bit.  
#define R_AARCH64_MOVW_UABS_G0  263  // Dir. MOVZ imm. from bits 15:0.  
#define R_AARCH64_MOVW_UABS_G0_NC 264  // Likewise for MOVK; no check.  
#define R_AARCH64_MOVW_UABS_G1  265  // Dir. MOVZ imm. from bits 31:16.  
#define R_AARCH64_MOVW_UABS_G1_NC 266  // Likewise for MOVK; no check.  
#define R_AARCH64_MOVW_UABS_G2  267  // Dir. MOVZ imm. from bits 47:32.  
#define R_AARCH64_MOVW_UABS_G2_NC 268  // Likewise for MOVK; no check.  
#define R_AARCH64_MOVW_UABS_G3  269  // Dir. MOV{K,Z} imm. from 63:48.  
#define R_AARCH64_MOVW_SABS_G0  270  // Dir. MOV{N,Z} imm. from 15:0.  
#define R_AARCH64_MOVW_SABS_G1  271  // Dir. MOV{N,Z} imm. from 31:16.  
#define R_AARCH64_MOVW_SABS_G2  272  // Dir. MOV{N,Z} imm. from 47:32.  
#define R_AARCH64_LD_PREL_LO19  273  // PC-rel. LD imm. from bits 20:2.  
#define R_AARCH64_ADR_PREL_LO21  274  // PC-rel. ADR imm. from bits 20:0.  
#define R_AARCH64_ADR_PREL_PG_HI21 275  // Page-rel. ADRP imm. from 32:12.  
#define R_AARCH64_ADR_PREL_PG_HI21_NC 276 // Likewise; no overflow check.  
#define R_AARCH64_ADD_ABS_LO12_NC 277  // Dir. ADD imm. from bits 11:0.  
#define R_AARCH64_LDST8_ABS_LO12_NC 278  // Likewise for LD/ST; no check. 
#define R_AARCH64_TSTBR14  279  // PC-rel. TBZ/TBNZ imm. from 15:2.  
#define R_AARCH64_CONDBR19  280  // PC-rel. cond. br. imm. from 20:2. 
#define R_AARCH64_JUMP26  282  // PC-rel. B imm. from bits 27:2.  
#define R_AARCH64_CALL26  283  // Likewise for CALL.  
#define R_AARCH64_CALL_PLT  1100  // davecc: PC-relative call via PLT.
#define R_AARCH64_LDST16_ABS_LO12_NC 284 // Dir. ADD imm. from bits 11:1.  
#define R_AARCH64_LDST32_ABS_LO12_NC 285 // Likewise for bits 11:2.  
#define R_AARCH64_LDST64_ABS_LO12_NC 286 // Likewise for bits 11:3.  
#define R_AARCH64_MOVW_PREL_G0  287  // PC-rel. MOV{N,Z} imm. from 15:0.  
#define R_AARCH64_MOVW_PREL_G0_NC 288  // Likewise for MOVK; no check.  
#define R_AARCH64_MOVW_PREL_G1  289  // PC-rel. MOV{N,Z} imm. from 31:16. 
#define R_AARCH64_MOVW_PREL_G1_NC 290  // Likewise for MOVK; no check.  
#define R_AARCH64_MOVW_PREL_G2  291  // PC-rel. MOV{N,Z} imm. from 47:32. 
#define R_AARCH64_MOVW_PREL_G2_NC 292  // Likewise for MOVK; no check.  
#define R_AARCH64_MOVW_PREL_G3  293  // PC-rel. MOV{N,Z} imm. from 63:48. 
#define R_AARCH64_LDST128_ABS_LO12_NC 299 // Dir. ADD imm. from bits 11:4.  
#define R_AARCH64_MOVW_GOTOFF_G0 300  // GOT-rel. off. MOV{N,Z} imm. 15:0. 
#define R_AARCH64_MOVW_GOTOFF_G0_NC 301  // Likewise for MOVK; no check.  
#define R_AARCH64_MOVW_GOTOFF_G1 302  // GOT-rel. o. MOV{N,Z} imm. 31:16.  
#define R_AARCH64_MOVW_GOTOFF_G1_NC 303  // Likewise for MOVK; no check.  
#define R_AARCH64_MOVW_GOTOFF_G2 304  // GOT-rel. o. MOV{N,Z} imm. 47:32.  
#define R_AARCH64_MOVW_GOTOFF_G2_NC 305  // Likewise for MOVK; no check.  
#define R_AARCH64_MOVW_GOTOFF_G3 306  // GOT-rel. o. MOV{N,Z} imm. 63:48.  
#define R_AARCH64_GOTREL64  307  // GOT-relative 64-bit.  
#define R_AARCH64_GOTREL32  308  // GOT-relative 32-bit.  
#define R_AARCH64_GOT_LD_PREL19  309  // PC-rel. GOT off. load imm. 20:2.  
#define R_AARCH64_LD64_GOTOFF_LO15 310  // GOT-rel. off. LD/ST imm. 14:3.  
#define R_AARCH64_ADR_GOT_PAGE  311  // P-page-rel. GOT off. ADRP 32:12.  
#define R_AARCH64_LD64_GOT_LO12_NC 312  // Dir. GOT off. LD/ST imm. 11:3.  
#define R_AARCH64_LD64_GOTPAGE_LO15 313  // GOT-page-rel. GOT off. LD/ST 14:3 
#define R_AARCH64_TLSGD_ADR_PREL21 512  // PC-relative ADR imm. 20:0.  
#define R_AARCH64_TLSGD_ADR_PAGE21 513  // page-rel. ADRP imm. 32:12.  
#define R_AARCH64_TLSGD_ADD_LO12_NC 514  // direct ADD imm. from 11:0.  
#define R_AARCH64_TLSGD_MOVW_G1  515  // GOT-rel. MOV{N,Z} 31:16.  
#define R_AARCH64_TLSGD_MOVW_G0_NC 516  // GOT-rel. MOVK imm. 15:0.  
#define R_AARCH64_TLSLD_ADR_PREL21 517  // Like 512; local dynamic model.  
#define R_AARCH64_TLSLD_ADR_PAGE21 518  // Like 513; local dynamic model.  
#define R_AARCH64_TLSLD_ADD_LO12_NC 519  // Like 514; local dynamic model.  
#define R_AARCH64_TLSLD_MOVW_G1  520  // Like 515; local dynamic model.  
#define R_AARCH64_TLSLD_MOVW_G0_NC 521  // Like 516; local dynamic model.  
#define R_AARCH64_TLSLD_LD_PREL19 522  // TLS PC-rel. load imm. 20:2.  
#define R_AARCH64_TLSLD_MOVW_DTPREL_G2 523 // TLS DTP-rel. MOV{N,Z} 47:32.  
#define R_AARCH64_TLSLD_MOVW_DTPREL_G1 524 // TLS DTP-rel. MOV{N,Z} 31:16.  
#define R_AARCH64_TLSLD_MOVW_DTPREL_G1_NC 525 // Likewise; MOVK; no check.  
#define R_AARCH64_TLSLD_MOVW_DTPREL_G0 526 // TLS DTP-rel. MOV{N,Z} 15:0.  
#define R_AARCH64_TLSLD_MOVW_DTPREL_G0_NC 527 // Likewise; MOVK; no check.  
#define R_AARCH64_TLSLD_ADD_DTPREL_HI12 528 // DTP-rel. ADD imm. from 23:12. 
#define R_AARCH64_TLSLD_ADD_DTPREL_LO12 529 // DTP-rel. ADD imm. from 11:0.  
#define R_AARCH64_TLSLD_ADD_DTPREL_LO12_NC 530 // Likewise; no ovfl. check.  
#define R_AARCH64_TLSLD_LDST8_DTPREL_LO12 531 // DTP-rel. LD/ST imm. 11:0.  
#define R_AARCH64_TLSLD_LDST8_DTPREL_LO12_NC 532 // Likewise; no check.  
#define R_AARCH64_TLSLD_LDST16_DTPREL_LO12 533 // DTP-rel. LD/ST imm. 11:1.  
#define R_AARCH64_TLSLD_LDST16_DTPREL_LO12_NC 534 // Likewise; no check.  
#define R_AARCH64_TLSLD_LDST32_DTPREL_LO12 535 // DTP-rel. LD/ST imm. 11:2.  
#define R_AARCH64_TLSLD_LDST32_DTPREL_LO12_NC 536 // Likewise; no check.  
#define R_AARCH64_TLSLD_LDST64_DTPREL_LO12 537 // DTP-rel. LD/ST imm. 11:3.  
#define R_AARCH64_TLSLD_LDST64_DTPREL_LO12_NC 538 // Likewise; no check.  
#define R_AARCH64_TLSIE_MOVW_GOTTPREL_G1 539 // GOT-rel. MOV{N,Z} 31:16.  
#define R_AARCH64_TLSIE_MOVW_GOTTPREL_G0_NC 540 // GOT-rel. MOVK 15:0.  
#define R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 541 // Page-rel. ADRP 32:12.  
#define R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC 542 // Direct LD off. 11:3.  
#define R_AARCH64_TLSIE_LD_GOTTPREL_PREL19 543 // PC-rel. load imm. 20:2.  
#define R_AARCH64_TLSLE_MOVW_TPREL_G2 544 // TLS TP-rel. MOV{N,Z} 47:32.  
#define R_AARCH64_TLSLE_MOVW_TPREL_G1 545 // TLS TP-rel. MOV{N,Z} 31:16.  
#define R_AARCH64_TLSLE_MOVW_TPREL_G1_NC 546 // Likewise; MOVK; no check.  
#define R_AARCH64_TLSLE_MOVW_TPREL_G0 547 // TLS TP-rel. MOV{N,Z} 15:0.  
#define R_AARCH64_TLSLE_MOVW_TPREL_G0_NC 548 // Likewise; MOVK; no check.  
#define R_AARCH64_TLSLE_ADD_TPREL_HI12 549 // TP-rel. ADD imm. 23:12.  
#define R_AARCH64_TLSLE_ADD_TPREL_LO12 550 // TP-rel. ADD imm. 11:0.  
#define R_AARCH64_TLSLE_ADD_TPREL_LO12_NC 551 // Likewise; no ovfl. check.  
#define R_AARCH64_TLSLE_LDST8_TPREL_LO12 552 // TP-rel. LD/ST off. 11:0.  
#define R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC 553 // Likewise; no ovfl. check. 
#define R_AARCH64_TLSLE_LDST16_TPREL_LO12 554 // TP-rel. LD/ST off. 11:1.  
#define R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC 555 // Likewise; no check.  
#define R_AARCH64_TLSLE_LDST32_TPREL_LO12 556 // TP-rel. LD/ST off. 11:2.  
#define R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC 557 // Likewise; no check.  
#define R_AARCH64_TLSLE_LDST64_TPREL_LO12 558 // TP-rel. LD/ST off. 11:3.  
#define R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC 559 // Likewise; no check.  
#define R_AARCH64_TLSDESC_LD_PREL19 560  // PC-rel. load immediate 20:2.  
#define R_AARCH64_TLSDESC_ADR_PREL21 561 // PC-rel. ADR immediate 20:0.  
#define R_AARCH64_TLSDESC_ADR_PAGE21 562 // Page-rel. ADRP imm. 32:12.  
#define R_AARCH64_TLSDESC_LD64_LO12 563  // Direct LD off. from 11:3.  
#define R_AARCH64_TLSDESC_ADD_LO12 564  // Direct ADD imm. from 11:0.  
#define R_AARCH64_TLSDESC_OFF_G1 565  // GOT-rel. MOV{N,Z} imm. 31:16.  
#define R_AARCH64_TLSDESC_OFF_G0_NC 566  // GOT-rel. MOVK imm. 15:0; no ck.  
#define R_AARCH64_TLSDESC_LDR  567  // Relax LDR.  
#define R_AARCH64_TLSDESC_ADD  568  // Relax ADD.  
#define R_AARCH64_TLSDESC_CALL  569  // Relax BLR.  
#define R_AARCH64_TLSLE_LDST128_TPREL_LO12 570 // TP-rel. LD/ST off. 11:4.  
#define R_AARCH64_TLSLE_LDST128_TPREL_LO12_NC 571 // Likewise; no check.  
#define R_AARCH64_TLSLD_LDST128_DTPREL_LO12 572 // DTP-rel. LD/ST imm. 11:4. 
#define R_AARCH64_TLSLD_LDST128_DTPREL_LO12_NC 573 // Likewise; no check.  
#define R_AARCH64_COPY         1024  // Copy symbol at runtime.  
#define R_AARCH64_GLOB_DAT     1025  // Create GOT entry.  
#define R_AARCH64_JUMP_SLOT    1026  // Create PLT entry.  
#define R_AARCH64_RELATIVE     1027  // Adjust by program base.  
#define R_AARCH64_TLS_DTPMOD   1028  // Module number, 64 bit.  
#define R_AARCH64_TLS_DTPREL   1029  // Module-relative offset, 64 bit.  
#define R_AARCH64_TLS_TPREL    1030  // TP-relative offset, 64 bit.  
// ELF TLS Variant I reserves a 16-byte TCB at TPIDR_EL0; static TLS follows.
#define AARCH64_TLS_TCB_SIZE 16
#define RISCV_TLS_TCB_SIZE 16
#define R_AARCH64_TLSDESC      1031  // TLS Descriptor.  
#define R_AARCH64_IRELATIVE  1032  // STT_GNU_IFUNC relocation.  


#define ELF_MACHINE_TYPE_PCODE 6500
#define ELF_MACHINE_TYPE_RISC_V 243
#define ELF_MACHINE_TYPE_XTENSA 94
#define ELF_MACHINE_TYPEW65C02 6502
#define ELF_MACHINE_TYPE_AARCH64  183
#define ELF_MACHINE_TYPE_ARM 40
#define ELF_MACHINE_TYPE_X86_64 62
#define ELF_MACHINE_TYPE_X86 3
#define ELF_MACHINE_TYPE_386 ELF_MACHINE_TYPE_X86
#define ELF_MACHINE_TYPE_BPF 247

// Linux eBPF relocation types (ELFCLASS64, little-endian / bpfel).
#define R_BPF_NONE 0
#define R_BPF_64_64 1
#define R_BPF_64_ABS64 2
#define R_BPF_64_ABS32 3
#define R_BPF_64_NODYLD32 4
#define R_BPF_64_32 10

// Xtensa ELF relocations used by the ESP32 target.
#define R_XTENSA_NONE 0
#define R_XTENSA_32 1
#define R_XTENSA_RTLD 2
#define R_XTENSA_GLOB_DAT 3
#define R_XTENSA_JMP_SLOT 4
#define R_XTENSA_RELATIVE 5
#define R_XTENSA_PLT 6
#define R_XTENSA_OP0 8
#define R_XTENSA_OP1 9
#define R_XTENSA_OP2 10
#define R_XTENSA_ASM_EXPAND 11
#define R_XTENSA_ASM_SIMPLIFY 12
#define R_XTENSA_32_PCREL 14
#define R_XTENSA_SLOT0_OP 20

// ARM ELF flags.
#define EF_ARM_EABI_VER5 0x05000000
#define EF_ARM_ABI_FLOAT_HARD 0x00000400
#define EF_ARM_EABI_FLAGS 0x0502

// ARM relocation types (EABI).
#define R_ARM_NONE 0
#define R_ARM_PC24 1
#define R_ARM_ABS32 2
#define R_ARM_REL32 3
#define R_ARM_LDR_PC_G0 4
#define R_ARM_ADD8 8
#define R_ARM_ADD16 9
#define R_ARM_ADD32 10
#define R_ARM_SUB8 11
#define R_ARM_SUB16 12
#define R_ARM_SUB32 13
#define R_ARM_TLS_DTPMOD32 17
#define R_ARM_TLS_DTPREL32 18
#define R_ARM_TLS_TPOFF32 19
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
#define R_ARM_GOT_BREL 26
#define R_ARM_PLT32 27
#define R_ARM_CALL 28
#define R_ARM_JUMP24 29
#define R_ARM_TLS_GD32 104
#define R_ARM_TLS_LDM32 105
#define R_ARM_TLS_LDO32 106
#define R_ARM_TLS_IE32 107
#define R_ARM_TLS_LE32 108
// ELF TLS Variant I reserves two words at the ARM thread pointer.
#define ARM_TLS_TCB_SIZE 8
#define R_ARM_CALL_PLT R_ARM_PLT32
#define R_ARM_32 R_ARM_ABS32
#define R_ARM_TARGET1 38
#define R_ARM_TARGET2 41
#define R_ARM_PREL31 42
#define R_ARM_V4BX 40
#define EXIDX_CANTUNWIND 1
#define R_ARM_MOVW_ABS_NC 43
#define R_ARM_MOVT_ABS 44
#define R_ARM_MOVW_PREL_NC 45
#define R_ARM_MOVT_PREL 46
#define R_ARM_GOT_PREL 96
#define R_ARM_COPY 20

// i386 relocation types.
#define R_386_NONE 0
#define R_386_32 1
#define R_386_PC32 2
#define R_386_GOT32 3
#define R_386_PLT32 4
#define R_386_COPY 5
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8
#define R_386_GOTOFF 9
#define R_386_GOTPC 10
#define R_386_32PLT 11
#define R_386_TLS_LE 17
#define R_386_GOT32X 43
#define I386_TLS_TP_SLOT_SIZE 8

// x86-64 relocation types.
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_GOT32 3
#define R_X86_64_PLT32 4
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_GOTPCREL 9
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_16 12
#define R_X86_64_PC16 13
#define R_X86_64_PC8 14
#define R_X86_64_PC64 24
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define R_X86_64_TLSGD 19
#define R_X86_64_TLSLD 20
#define R_X86_64_DTPOFF32 21
#define R_X86_64_GOTTPOFF 22
#define R_X86_64_TPOFF32 23
// Reserved bytes at the start of each thread's TLS block for %fs:0.
#define X86_64_TLS_TP_SLOT_SIZE 8

// A program header.
typedef struct {
  ELF_Word type;
  ELF_Word flags;
  ELF_Off offset;     // Segment file offset.
  ELF_Addr vaddr;     // Segment virtual address.
  ELF_Addr paddr;     // Segment physical address.
  ELF_Xword filesz;    // Segment size in file.
  ELF_Xword memsz;    // Segment size in memory.
  ELF_Xword align;    // Segment alignment, file & memory.
} ELFProgramHeader;

// Section type.  Each section has a specific type and therefore contains
// information in a specific format.  For example, the symtab section contains
// ELFSymbol structs.
#define SHT(x) kELFSectionType_##x
typedef enum {
  SHT(null),         // No type.
  SHT(progbits),     // Contains code or data.
  SHT(symtab),       // Symbol table.
  SHT(strtab),       // String table.
  SHT(rela),         // Relocations with addend present.
  SHT(hash),         // A symbol hash table.
  SHT(dynamic),      // Dynamic section for shared objects.
  SHT(note),         // A general note.
  SHT(nobits),       // No data is in section.
  SHT(rel),          // Relocations without addend.
  SHT(shlib),        // Shared library information.
  SHT(dynsym),       // Dynamic symbol table.
  SHT(init_array) = 14,     // Array of initialization function pointers.
  SHT(fini_array) = 15,     // Array of termination function pointers.
  SHT(preinit_array) = 16,  // Array of pre-initialization function pointers.
  SHT(gnu_hash) = 0x6ffffff6,     // GNU hash table.
  SHT(ARM_EXIDX) = 0x70000001,    // ARM exception index table.
  SHT(num),
} ELFSectionType;

// Section flags.  These are ORed together.
#define SHF(x) kELFSectionFlag_##x
typedef enum {
  SHF(write) = 0x1,                  // Section is writeable.
  SHF(alloc) = 0x2,                  // Memory allocated in executable.
  SHF(execinstr) = 0x4,              // Contains executable code.
                                     // 0x08 is missing.
  SHF(merge) = 0x10,                 // Mergeable.
  SHF(strings) = 0x20,               // Contains strings.
  SHF(tls) = (1 << 10),              // Thread Local Storage.
  SHF(link_order) = (1 << 7),        // Preserve section order for linking.
  SHF(gnu_retain) = 0x200000,        // Do not GC this section (--gc-sections).
} ELFSectionFlags;

// A section header.
typedef struct  {
  ELF_Word name;        // Section name, index in section name table.
  ELF_Word type;        // Section type.
  ELF_Xword flags;
  ELF_Addr addr;        // Section virtual address.
  ELF_Off offset;       // Offset into file.
  ELF_Xword size;       // Size of section on disk.
  ELF_Word link;        // Linked section index (if any).
  ELF_Word info;        // Special info about section.
  ELF_Xword addralign;  // Alignment requirements.
  ELF_Xword entsize;    // Size of a fixed size entry insection (or 0).
} ELFSectionHeader;


// Reserved section indexes.
#define SHN_LORESERVE 0xff00  // Start of reserved section indices.
#define SHN_ABS 0xfff1        // Absolute.
#define SHN_COM 0xfff2        // Common.

// Dynamic section.

// Dynamic section tags.
#define DT(x) kELFDynamicTag_##x
typedef enum {
  DT(null)        = 0,
  DT(needed)      = 1,
  DT(pltrelsz)    = 2,
  DT(pltgot)      = 3,
  DT(hash)        = 4,
  DT(strtab)      = 5,
  DT(symtab)      = 6,
  DT(rela)        = 7,
  DT(relasz)      = 8,
  DT(relaent)     = 9,
  DT(strsz)       = 10,
  DT(syment)      = 11,
  DT(init)        = 12,
  DT(fini)        = 13,
  DT(soname)      = 14,
  DT(rpath)       = 15,
  DT(symbolic)    = 16,
  DT(rel)         = 17,
  DT(relsz)       = 18,
  DT(relent)      = 19,
  DT(pltrel)      = 20,
  DT(debug)       = 21,
  DT(textrel)     = 22,
  DT(jmprel)      = 23,
  DT(bind_now)    = 24,
  DT(init_array)  = 25,
  DT(fini_array)  = 26,
  DT(init_arraysz) = 27,
  DT(fini_arraysz) = 28,
  DT(runpath)     = 29,
  DT(flags)       = 30,
  DT(preinit_array) = 32,
  DT(preinit_arraysz) = 33,
  DT(maxpostags)  = 34,
  // OS extensions in here.
  DT(checksum)    = 0x6ffffdf8,
  DT(pltpadsz)    = 0x6ffffdf9,
  DT(moveent)     = 0x6ffffdfa,
  DT(movsz)       = 0x6ffffdfb,
  DT(posflag_1)   = 0x6ffffdfd,
  DT(syminsz)     = 0x6ffffdfe,
  DT(syminent)    = 0x6ffffdff,
  DT(gnu_hash)    = 0x6ffffef5,
  DT(config)      = 0x6ffffefa,
  DT(depaudit)    = 0x6ffffefb,
  DT(audit)       = 0x6ffffefc,
  DT(pltpad)      = 0x6ffffefd,
  DT(movetab)     = 0x6ffffefe,
  DT(syminfo)     = 0x6ffffeff,
  DT(relacount)   = 0x6ffffff9,
  DT(relcount)    = 0x6ffffffa,
  DT(flags_1)     = 0x6ffffffb,
  DT(verdef)      = 0x6ffffffc,
  DT(verdefnum)   = 0x6ffffffd,
  DT(verneed)     = 0x6ffffffe,
  DT(verneednum)  = 0x6fffffff,
  DT(auxilliary)  = 0x7ffffffd,
  DT(used)        = 0x7ffffffe,
  DT(filter)      = 0x7fffffff,
} ELFDynamicTag;

#define DF_TEXTREL 0x4
#define DF_BIND_NOW 0x8

typedef struct {
  ELF_Xword tag;
  union {
    ELF_Xword val;
    ELF_Addr ptr;
  } un;
} ELFDynamicSectionEntry;

// Header for ELF file.
typedef struct {
  uint8_t ident[16];      // "\x7fELF" + indentification bytes.
  ELF_Half type;          // Type of ELF file (ET macro).
  ELF_Half machine;       // Machine this is for (architecture).
  ELF_Word version;       // The value 1.
  ELF_Addr entry;         // Entry point virtual address.
  ELF_Off phoff;          // Program header table file offset.
  ELF_Off shoff;          // Section header table file offset.
  ELF_Word flags;         // Some flags.
  ELF_Half ehsize;        // Size of ELF header (this struct).
  ELF_Half phentsize;     // Size of a program header.
  ELF_Half phnum;         // Number of program headers.
  ELF_Half shentsize;     // Size of a section header.
  ELF_Half shnum;         // Number of sections.
  ELF_Half shstrndx;      // Section index for section name string table.
} ELFHeader;

// Indexes into 'ident' field in header.
#define	EI_MAG0 0
#define	EI_MAG1 1
#define	EI_MAG2 2
#define	EI_MAG3 3
#define	EI_CLASS 4
#define	EI_DATA 5
#define	EI_VERSION 6
#define	EI_OSABI 7
#define	EI_PAD 8

// Values for the EI_CLASS byte of the ident field.
#define ELFCLASS32 1
#define ELFCLASS64 2

// ===========================================================================
// ELF32 vs ELF64 support.
//
// All of the types and structures above are the ELF64 (LP64) on-disk layout.
// They are also used as the canonical in-memory representation throughout the
// compiler, linker and loaders.  ELF32 files use narrower fields and, for
// some structures, a different field order, so they need their own set of
// on-disk structures.
//
// The ELF reader/writer convert between these on-disk structures and the
// canonical (wide) in-memory structures above using the function pointers in
// the generic ELFFormatOps "class" (see elf_format.h).
// ===========================================================================

// ELF64 types.  These alias the canonical (wide) types above so that the
// ELF64-specific reader/writer functions can be written in terms of ELF64
// names while sharing the in-memory representation.
typedef ELF_Addr   ELF64_Addr;
typedef ELF_Half   ELF64_Half;
typedef ELF_Off    ELF64_Off;
typedef ELF_Sword  ELF64_Sword;
typedef ELF_Word   ELF64_Word;
typedef ELF_Xword  ELF64_Xword;
typedef ELF_Sxword ELF64_Sxword;

typedef ELFHeader              ELF64Header;
typedef ELFProgramHeader       ELF64ProgramHeader;
typedef ELFSectionHeader       ELF64SectionHeader;
typedef ELFSymbol              ELF64Symbol;
typedef ELFRelocation          ELF64Relocation;
typedef ELFDynamicSectionEntry ELF64DynamicSectionEntry;

// ELF32 types.
typedef uint32_t ELF32_Addr;
typedef uint16_t ELF32_Half;
typedef uint32_t ELF32_Off;
typedef int32_t  ELF32_Sword;
typedef uint32_t ELF32_Word;

// ELF32 file header.  The Addr/Off fields are 32 bits wide (so the header is
// 52 bytes rather than 64).
typedef struct {
  uint8_t ident[16];
  ELF32_Half type;
  ELF32_Half machine;
  ELF32_Word version;
  ELF32_Addr entry;
  ELF32_Off phoff;
  ELF32_Off shoff;
  ELF32_Word flags;
  ELF32_Half ehsize;
  ELF32_Half phentsize;
  ELF32_Half phnum;
  ELF32_Half shentsize;
  ELF32_Half shnum;
  ELF32_Half shstrndx;
} ELF32Header;

// ELF32 program header.  Note the 'flags' field comes last (unlike ELF64,
// where it is the second field).
typedef struct {
  ELF32_Word type;
  ELF32_Off offset;
  ELF32_Addr vaddr;
  ELF32_Addr paddr;
  ELF32_Word filesz;
  ELF32_Word memsz;
  ELF32_Word flags;
  ELF32_Word align;
} ELF32ProgramHeader;

// ELF32 section header.  All fields are 32 bits wide (40 bytes total).
typedef struct {
  ELF32_Word name;
  ELF32_Word type;
  ELF32_Word flags;
  ELF32_Addr addr;
  ELF32_Off offset;
  ELF32_Word size;
  ELF32_Word link;
  ELF32_Word info;
  ELF32_Word addralign;
  ELF32_Word entsize;
} ELF32SectionHeader;

// ELF32 symbol.  Note the field order differs from the ELF64 symbol: value and
// size come before info/other/shndx (16 bytes total).
typedef struct {
  ELF32_Word name;
  ELF32_Addr value;
  ELF32_Word size;
  uint8_t info;
  uint8_t other;
  ELF32_Half shndx;
} ELF32Symbol;

// ELF32 SHT_REL entry (8 bytes total).
typedef struct {
  ELF32_Addr offset;
  ELF32_Word info;
} ELF32Rel;

// ELF32 SHT_RELA entry (12 bytes total).
typedef struct {
  ELF32_Addr offset;
  ELF32_Word info;
  ELF32_Sword addend;
} ELF32Relocation;

// ELF32 dynamic section entry (8 bytes total).
typedef struct {
  ELF32_Sword tag;
  union {
    ELF32_Word val;
    ELF32_Addr ptr;
  } un;
} ELF32DynamicSectionEntry;

// Relocation 'info' field encoding.  In ELF64 the symbol index is the top
// 32 bits and the type is the bottom 32 bits.  In ELF32 the symbol index is
// the top 24 bits and the type is the bottom 8 bits.  The canonical in-memory
// representation always uses the ELF64 encoding; the ELF32 serialization
// re-encodes when writing/reading.
#define ELF64_R_SYM(info)           ((uint32_t)((info)>>32))
#define ELF64_R_TYPE(info)          ((uint32_t)(info))
#define ELF64_R_INFO(sym, type)     (((ELF_Xword)(sym)<<32)+(ELF_Xword)(type))

#define ELF32_R_SYM(info)           ((uint32_t)((info)>>8))
#define ELF32_R_TYPE(info)          ((uint8_t)(info))
#define ELF32_R_INFO(sym, type)     (((ELF32_Word)(sym)<<8)+((type)&0xff))

#endif // elf_h 
