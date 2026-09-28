//
//  elf_reader.h
//  c_compiler
//
//  Created by David Allison on 1/9/18.
//  Copyright © 2018 David Allison. All rights reserved.
//

#ifndef elf_reader_h
#define elf_reader_h

// This is a general purpose ELF (Execuable and Linkable Format) file reader.
#include <sys/stat.h>

#include <stdio.h>
#include "elf.h"
#include "elf_format.h"
#include "vector.h"
#include "dstring.h"
#include "hashtable.h"
#include "buffer.h"

typedef struct ELFReaderSection {
  ELFSectionHeader* header;
  String name;
  void* contents;               // Contents of section.
  uint64_t address;             // Address assigned to section.
  uint64_t offset;              // Offset into output section.
  int32_t output_section_index; // Section index in output.
  // Local symbol that marks this input section's start in a -r object.
  int32_t section_symbol_index;
  bool discarded;               // Unused allocatable section dropped by GC.
  Vector gc_refs;               // ELFReaderSection* this section relocates to.
} ELFReaderSection;

ELFReaderSection* NewELFReaderSection(void);
void ELFReaderSectionDelete(ELFReaderSection* section);

typedef struct {
  ELFHeader* header;          // Header (wide, canonical) for the ELF file.
  String filename;
  int64_t file_length;
  Vector sections;            // Vector of ELFReaderSection*.
  Vector segments;            // Vector of ELFProgramHeader*.
  const char* section_names;  // Section names string table mapped from file.
  struct stat file_stat;      // Result of stat call.

  // Format operations (ELF32 or ELF64) selected from the file's EI_CLASS byte.
  const ELFFormatOps* ops;
  // Base address of the file mapped into memory.  Section contents and string
  // tables are referenced directly from here; section/segment file offsets are
  // relative to this address.  For ELF64 the header and section headers point
  // directly into this mapping; for ELF32 they are decoded into owned wide
  // structures (see owns_decoded).
  const char* base;
  // True if 'header', the per-section headers and the segment headers were
  // decoded into heap-allocated wide structures (the ELF32 case) and therefore
  // need to be freed.
  bool owns_decoded;
} ELFReaderFile;

void ELFReaderFileInit(ELFReaderFile* elf, String* filename);
ELFReaderFile* NewELFReaderFile(String* filename);

bool ELFReaderFileRead(ELFReaderFile* elf, int64_t length, int64_t offset);
void ELFReaderFileDestruct(ELFReaderFile* elf);
void ELFReaderFileDelete(ELFReaderFile* elf);
ELFReaderSection* ELFReaderFileFindSection(ELFReaderFile* elf, const char* name);
void ELFReaderFileFindSectionsByType(ELFReaderFile* elf, int32_t type, Vector* output);

#endif /* elf_reader_h */
