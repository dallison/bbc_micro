//
//  elf_writer.h
//  c_compiler
//
//  Created by David Allison on 1/9/18.
//  Copyright © 2018 David Allison. All rights reserved.
//

#ifndef elf_writer_h
#define elf_writer_h

// This is a general purpose ELF (Executable and Linkable Format) file
// writer.  It can be used to construct ELF files from an assembler
// or linker.
#include "elf.h"
#include "elf_format.h"

// The section contents can come from different places.  If an
// assembler is creating the section the data will be in a Buffer.
// If the linker is creating it, the section contents are either
// in mapped memory or in the heap.  For fixed size memory regions
// the linker can create padding sections to fill out unused
// memory.
typedef enum {
  kSectionContentsBuffered,   // Data is in a Buffer.
  kSectionContentsRaw,        // Data is in memory.
  kSectionContentsMulti,      // Data is in multiple parts.
  kSectionContentsNobits,     // Nothing in this section, but size is valid.
  kSectionContentsPad,        // Pad with zeroes.
} ELFWriterSectionContentsDataLocation;

// If the data location is a buffer it may be empty (a .bss section)
// in which case the size field is the size of the data.  If the data
// comes from memory, the size field is the length of the data in memory.
typedef struct ELFWriterSectionContents {
  ELFWriterSectionContentsDataLocation data_location;        // Data location.
  union {
    Buffer buffered;     // Data contained in section.
    void* raw;           // Mapped or heap memory.
    Vector multi;        // Vector of ELFWriterSectionContents*.
  } data;
  size_t size;        // Size of section if data is raw or no buffer data.
} ELFWriterSectionContents;

void ELFWriterSectionContentsInit(ELFWriterSectionContents* contents, ELFWriterSectionContentsDataLocation location);
ELFWriterSectionContents* NewELFWriterSectionContents(ELFWriterSectionContentsDataLocation location);

void ELFWriterSectionContentsDestruct(ELFWriterSectionContents* contents);
size_t ELFWriterSectionContentsGetLength(ELFWriterSectionContents* contents);
void ELFWriterSectionContentsWrite(ELFWriterSectionContents* contents, FILE* fp);

// A section with header and other information.
typedef struct ELFWriterSection {
  String name;              // Section name as a string.
  ELFSectionHeader header;  // Header, exactly as it will appear in file.
  ELFWriterSectionContents* contents;     // Section contents (not owned).
  int32_t index;            // Index into section table.
  Vector* relocations;      // Relocations (if this is a relocation section).
  uint64_t address;             // Address assigned to section.
  uint64_t padding;          // Padding to next section.
  void* user_data;           // Opaque user data.
} ELFWriterSection;

void ELFWriterSectionDestruct(ELFWriterSection* section);
void ELFWriterSectionDelete(ELFWriterSection* section);

// A Segment (a.k.a Program Header) is a mapping to a set of sections.
typedef struct {
  ELFProgramHeader header;
  Vector sections;      // Pointers to ELFWriterSection.
} ELFWriterSegment;

ELFWriterSegment* NewELFWriterSegment(int32_t type, int32_t flags, int64_t alignment);
void ELFWriterSegmentDestruct(ELFWriterSegment* segment);
void ELFWriterSegmentDelete(ELFWriterSegment* segment);
void ELFWriterSegmentAddSection(ELFWriterSegment* segment, ELFWriterSection* section);

// Struct to link sections together by their 'link' or 'info' field.
// Sections are identified by their name.
typedef enum {
  kFixupFieldInfo,
  kFixupFieldLink,
} ELFWriterFixupField;

typedef struct {
  ELFWriterFixupField field;
  const char* from;
  const char* to;
} ELFWriterSectionFixup;

struct ELFWriterFile;

typedef void (*DynamicCallback)(struct ELFWriterFile* elf);

// A complete ELF file.
typedef struct ELFWriterFile {
  ELFHeader header;         // File header (as it appears on disk).
  Vector sections;          // All sections.
  Vector segments;          // All program segments (pointer to ELFSegment)
  Buffer string_table;      // String table (zero terminated strings).
  Vector symbol_table;      // Symbol table (ELFSymbol pointers).
  Buffer dyn_string_table;  // Dynamic String table (zero terminated strings).
  Vector dyn_symbol_table;  // Dynamic Symbol table (ELFSymbol pointers).
  Buffer section_names;     // Section names string table.
  int32_t last_local_symbol_index;  // Index of last local symbol.
  Vector section_fixups;    // Vector of ELFWriterSectionFixup*.
  
  // Function to call to handle dynamic libraries or exes (or NULL).
  DynamicCallback dynamic_callback;

  // Format operations (ELF32 or ELF64) used for all width-sensitive on-disk
  // sizes and serialization.  Set from the is64bit flag at init time.
  const ELFFormatOps* ops;
} ELFWriterFile;

void ELFWriterFileInit(ELFWriterFile* elf, ELFType type, int machine,
                       int flags, DynamicCallback dynamic_callback,
                       bool is64bit, bool isLittleEndian);
void ELFWriterFileWrite(ELFWriterFile* elf, FILE* fp);
void ELFWriterFileDestruct(ELFWriterFile* elf);
ELFWriterFile* NewELFWriterFileFromFile(FILE* fp);

ELFWriterSection* ELFWriterAddSection(ELFWriterFile* elf, String* name, int32_t type, int64_t flags,
                          int64_t alignment, ELFWriterSectionContents* contents, uint64_t address);
ELFWriterSection* ELFWriterAddStandardSection(ELFWriterFile* elf, const char* name,
                               ELFSectionType type,
                               ELFSectionFlags flags);

ELFSymbol* ELFWriterAddSectionSymbol(ELFWriterFile* elf, String* name,
                                     int32_t index);

ELF_Word ELFWriterAddSectionName(ELFWriterFile* elf, const char* name);
ELFWriterSection* ELFWriterFindSection(ELFWriterFile* elf, const char* name);

ELF_Word ELFWriterAddString(ELFWriterFile* elf, String* str);
ELF_Word ELFWriterAddSectionString(ELFWriterFile* elf, Buffer* strtab, String* str);
ELF_Word ELFWriterAddRawString(ELFWriterFile* elf, const char* str);
ELFSymbol* ELFWriterAddSymbol(ELFWriterFile* elf, String* name, int32_t section_index, int32_t symbol_type,
                        int32_t symbol_binding, int64_t size, int64_t value, int32_t* index);
ELFSymbol* ELFWriterAddFileSymbol(ELFWriterFile* elf, String* filename);
void ELFWriterInitRelocation(ELFRelocation* r,
                             int64_t offset,
                             int32_t symbol_index,
                             int64_t addend,
                             int32_t type);
void ELFWriterAddRelocation(ELFWriterFile* elf, int32_t section_index, int64_t offset, int32_t symbol_index, int32_t type);
void ELFWriterAddRelocationWithAddend(ELFWriterFile* elf, int32_t section_index, int64_t offset, int32_t symbol_index, int64_t addend, int32_t type);
void ELFWriterInsertRelocation(ELFWriterFile* elf, int32_t section_index, ELFRelocation* reloc);
void ELFWriterAddSectionFixup(ELFWriterFile* elf, ELFWriterFixupField field, ELFWriterSection* from, ELFWriterSection* to);
void ELFWriterAddSectionFixupByName(ELFWriterFile* elf, ELFWriterFixupField field, const char* from, const char* to);

void ELFWriterFixupSections(ELFWriterFile* elf);

void ELFSymbolInit(ELFSymbol* sym,
                   ELF_Word name_offset,
                   int32_t section_index, int32_t symbol_type,
                   int32_t symbol_binding, int64_t size,
                   int64_t value);

ELFSymbol* NewELFSymbol(ELF_Word name_offset,
                        int32_t section_index, int32_t symbol_type,
                        int32_t symbol_binding, int64_t size,
                        int64_t value);

#endif /* elf_writer_h */
