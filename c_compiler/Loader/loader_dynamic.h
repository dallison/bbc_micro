//
//  dynamic.h
//  common_utils
//
//  Created by David Allison on 2/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#ifndef dynamic_h
#define dynamic_h

#include "elf.h"
#include "hashtable.h"
#include "map.h"

struct LoadedDynamicLibrary;
struct Loader;

extern bool print_libraries_only;

// A MappedSegment is a mapped region of memory that should be unmapped when
// we are done.
typedef struct {
  void* address;        // Virtual address.
  int64_t length;       // Length in bytes.
} MappedSegment;

// Dynamic section contents.  This is mapped in from a loaded
// dso and contains information necessary for linking and loading
// dynamic objects.
typedef struct {
  ELFDynamicSectionEntry entries[1];    // Variable size.
} DynamicSection;

// Standardized GNU hash table header.
typedef struct  {
  uint32_t num_buckets;
  uint32_t symoffset;
  uint32_t bloom_size;
  uint32_t bloom_shift;
} DynamicLoaderGNUHashTableHeader;

// There are two things we need to keep about loaded libraries:
// 1. Whether they are loaded or not, with fast lookup.
// 2. A list ordered by the rules of ELF, used when searching
//    for symbols.
typedef struct {
  Map loaded_libraries;
  Vector search;
} DynamicLibraryRegistry;


void DynamicLibraryRegistryInit(DynamicLibraryRegistry* reg);
void DynamicLibraryRegistryDestruct(DynamicLibraryRegistry* reg);
void DynamicLibraryRegistryInsert(DynamicLibraryRegistry* reg,
                                  struct LoadedDynamicLibrary* lib);

// This is a loaded dynamic library.  The contents point to memory into
// which the ELF file is mapped.
typedef struct LoadedDynamicLibrary {
  String libname;         // Original library name.
  String filename;        // File loaded.
  struct Loader* loader;
  const void* addr;                 // Address library loaded at.
  size_t length;              // Length of loaded library.
  int fd;
  uint64_t load_address;
  const ELFHeader* header;
  const ELFProgramHeader* program_headers;
  const ELFSectionHeader* section_headers;
  const DynamicSection* dynamic;
  const DynamicLoaderGNUHashTableHeader* gnu_hash;
  Vector runtime_search_path;   // Vector of String*.
  
  // Dynamic symbol table and string table.  Must be present.
  const ELFSymbol* dynsym;
  const char* dynstr;
  int64_t num_dynamic_symbols;

  // Regular symbol and string table.  Used only for reverse symbol
  // lookup.  May not be present.
  bool load_symbol_table;
  const ELFSymbol* symtab;
  const char* strtab;
  int64_t num_symtab_symbols;
  bool dynamic_section_relocated;
  // True when the header, program headers and section headers were decoded
  // from an ELF32 file into heap-allocated (wide) structures and therefore
  // need to be freed.  For ELF64 these point directly into the mapped file and
  // must not be freed.
  bool owns_decoded;
  // True when the dynamic section and dynamic symbol table were also decoded
  // into heap memory.
  bool owns_dynamic_tables;
  // Canonical relocation arrays decoded from ELF32 REL/RELA sections.
  ELFRelocation* decoded_data_relocations;
  ELFRelocation* decoded_plt_relocations;
  Vector mapped_segments;
} LoadedDynamicLibrary;

LoadedDynamicLibrary* NewLoadedDynamicLibrary(const char* libname,
                                              struct Loader* loader);


void LoadedDynamicLibraryDestruct(LoadedDynamicLibrary* lib);
void LoadedDynamicLibraryDelete(LoadedDynamicLibrary* lib);

bool LoadedDynamicLibraryLoad(LoadedDynamicLibrary* lib,
                              DynamicLibraryRegistry* registry,
                              Vector* search_path,
                              uint64_t load_address,
                              uint64_t* end_of_library);

void LoadedDynamicLibraryRelocate(struct Loader* loader,
                                  LoadedDynamicLibrary* lib,
                                  DynamicLibraryRegistry* registry,
                                  bool lazy);

const ELFSymbol* LoadedDynamicLibraryFindSymbol(LoadedDynamicLibrary* lib,
                                                    const char* name);
uint64_t LoadedDynamicLibraryLoadSegments(LoadedDynamicLibrary* lib,
                                          uint64_t load_address, uint64_t* next_available_address);

bool LoadedDynamicLibraryLookupSymbolByAddress(LoadedDynamicLibrary* lib,
                                               uint64_t address,
                                               const char** name,
                                               uint64_t* start,
                                               uint64_t* length);

LoadedDynamicLibrary* DynamicLoaderFindLibrary(DynamicLibraryRegistry* registry,
                                               String* filename);
bool DynamicLoaderFindSymbol(DynamicLibraryRegistry* registry,
                             const char* name,
                             const ELFSymbol** symbol,
                             LoadedDynamicLibrary** library);

const void* DynamicLoaderFindDynamicSectionAddressEntry(const LoadedDynamicLibrary* lib,
                                                        ELFDynamicTag tag);

int64_t DynamicLoaderFindDynamicSectionOffsetEntry(const LoadedDynamicLibrary* lib,
                                                   ELFDynamicTag tag);

uint32_t DynamicLoaderGNUHash(const char* name);
uint64_t DynamicLoaderBloomBits64(uint32_t hash);

// Given an address, lookup the symbol it corresponds to.  Return true if found
// and set *name to the name and *length to the length of the symbol.
bool DynamicLoaderLookupSymbolByAddress(DynamicLibraryRegistry* registry,
                                        uint64_t address,
                                        const char** name,
                                        uint64_t* start,
                                        uint64_t* length);

#endif /* dynamic_h */
