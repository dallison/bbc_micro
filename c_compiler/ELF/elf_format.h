//
//  elf_format.h
//  c_compiler
//
//  Generic ELF "class" abstracting over the on-disk differences between
//  ELF32 and ELF64.
//

#ifndef elf_format_h
#define elf_format_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "elf.h"

// The compiler, linker and loaders use the wide (ELF64) structures defined in
// elf.h as the canonical in-memory representation.  ELFFormatOps is a set of
// function pointers and sizes that know how to serialize those structures to,
// and deserialize them from, either the ELF32 or the ELF64 on-disk layout.
//
// An ELF reader or writer holds a pointer to one of the two static
// ELFFormatOps instances (selected by the file's class) and indirects through
// it for all width-sensitive operations.  The width-independent logic (section
// management, symbol tables, fixups, ...) is shared.
typedef struct ELFFormatOps {
  bool is_64_bit;

  // On-disk sizes of the various structures.
  size_t header_size;
  size_t section_header_size;
  size_t program_header_size;
  size_t symbol_size;
  size_t relocation_size;
  size_t dynamic_entry_size;

  // Serialization: write the canonical (wide) structure to 'fp' using this
  // format's on-disk layout.
  void (*WriteHeader)(const ELFHeader* hdr, FILE* fp);
  void (*WriteSectionHeader)(const ELFSectionHeader* hdr, FILE* fp);
  void (*WriteProgramHeader)(const ELFProgramHeader* hdr, FILE* fp);
  void (*WriteSymbol)(const ELFSymbol* sym, FILE* fp);
  void (*WriteRelocation)(const ELFRelocation* rel, FILE* fp);
  void (*WriteDynamicEntry)(const ELFDynamicSectionEntry* dyn, FILE* fp);

  // Deserialization: decode the on-disk structure at 'src' into the canonical
  // (wide) structure '*dst'.
  void (*ReadHeader)(ELFHeader* dst, const void* src);
  void (*ReadSectionHeader)(ELFSectionHeader* dst, const void* src);
  void (*ReadProgramHeader)(ELFProgramHeader* dst, const void* src);
  void (*ReadSymbol)(ELFSymbol* dst, const void* src);
  void (*ReadRelocation)(ELFRelocation* dst, const void* src);

  // Relocation 'info' field encode/decode (the bit layout differs by width).
  // The canonical in-memory representation always uses the ELF64 encoding;
  // these helpers operate on the on-disk encoding for this format.
  ELF_Xword (*EncodeRelocationInfo)(uint32_t symbol, uint32_t type);
  uint32_t (*DecodeRelocationSym)(ELF_Xword info);
  uint32_t (*DecodeRelocationType)(ELF_Xword info);
} ELFFormatOps;

// Return the ops for the requested width.  The returned pointer is to a static
// instance and must not be freed.
const ELFFormatOps* ELFFormatOpsFor(bool is_64_bit);

// Decode either SHT_REL or SHT_RELA without reading beyond the on-disk entry.
void ELFFormatReadRelocation(const ELFFormatOps* ops, bool is_rela,
                             ELFRelocation* dst, const void* src);

#endif /* elf_format_h */
