//
//  loader.h
//
//  Created by David Allison on 1/18/18.
//  Copyright © 2018 David Allison. All rights reserved.
//

#ifndef loader_h
#define loader_h

#include "elf_reader.h"
#include <stdio.h>
#include "hashtable.h"
#include "map.h"
#include "loader_dynamic.h"
#include <stdarg.h>

struct LoaderLifecycleState;

struct LoaderArchitecture;

// A Region is a mapped region of memory that should be unmapped when
// we are done.
typedef struct {
  void* address;        // Virtual address.
  int64_t offset;
  int64_t length;       // Length in bytes.
  Vector sections;      // Sections in this region (ELFReaderSection*).
  ELFProgramHeader* segment;
  LoadedDynamicLibrary* owner;
} Region;

Region* NewRegion(void* addr, int64_t offset, int64_t length,
                  ELFProgramHeader* segment,
                  LoadedDynamicLibrary* owner);
void RegionDestruct(Region* region);

// A symbol scope is a region of memory that corresponds
// to a given symbol name.  This is used to cache a symbol
// lookup when inside a function where the symbol will not
// change until a call is made.
typedef struct {
  uint64_t start;
  uint64_t end;
  const char* name;
} SymbolScope;

// The runtime heap (used by malloc) lives immediately above the program's last
// writable section, starting at the linker-defined '_end' symbol.  The libc
// malloc for targets without a heap-lock syscall (p-code, 6502) assumes a
// contiguous block of memory there without ever calling brk/mmap to obtain it,
// so the loader must reserve and map that space.  Without it the very first
// malloc writes just past the last mapped page and faults.
#define LOADER_HEAP_RESERVE (4 * 1024 * 1024)

// Loader flags.
#define LOADER_MAP_SYMTAB 1      // Load symbol table.
#define LOADER_LAZY_RESOLVE 2    // Use lazy PLT resolution.
#define LOADER_WRITEABLE_TEXT 4  // Map the text writeable.
#define LOADER_EXECUTABLE_MAPPING 8  // Map text segments OS-executable.

typedef struct LoaderTlsInfo {
  bool present;
  uint64_t template_addr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
  uint64_t file_offset;
  void* main_thread_block;
  size_t block_size;
  uint64_t fs_base;
  uint64_t tp_base;
} LoaderTlsInfo;

typedef struct {
  uint64_t load_address;
  const ELFSymbol* symtab;
  const char* strtab;
  int64_t num_symtab_symbols;
  Vector symbols_by_addr;       // Sorted by address.
  Map symbols_by_name;
} StaticSymbolTable;

// A loader loads ELF files into memory based on their contents.  The
// file header contains everything we need to find the loadable regions
// and load them into memory at the correct addresses.
typedef struct Loader {
  String filename;
  bool is_static;
  int32_t flags;
  struct LoaderArchitecture* arch;
  void* arch_data;
  String origin;
  String resolved_origin;
  
  ELFReaderFile* elf_file;
  uint64_t main_address;
  Vector regions;
  DynamicSection* dynamic;
  Vector library_search_path;
  LoadedDynamicLibrary* dynamic_lib;
  DynamicLibraryRegistry loaded_libraries;
  SymbolScope current_symbol;
  StaticSymbolTable static_symbol_table;
  LoaderTlsInfo tls;
  struct LoaderLifecycleState* lifecycle;
} Loader;

bool LoaderInitFromFile(Loader* loader, String* filename,
                        int32_t flags,
                        struct LoaderArchitecture* arch, void* arch_data,
                        const char* initial_path);
void LoaderDestruct(Loader* loader);
SymbolScope* LoaderFindSymbolAndCacheResult(Loader* loader,
                         uint64_t address);
bool LoaderFindSymbol(Loader* loader,
                              uint64_t address, SymbolScope* symbol);
uint64_t LoaderLookupSymbol(Loader* loader, const char* name);

void LoaderSetCurrentSymbol(Loader* loader,
                          uint64_t address, uint64_t length, const char* name);
SymbolScope* LoaderGetCurrentSymbol(Loader* loader);

void LoaderError(const char* error, ...);

void VLoaderError(const char* error, va_list ap);

int LoaderNumErrors(void);

bool LoaderLinkedAddressToRuntime(Loader* loader,
                                  const LoadedDynamicLibrary* lib,
                                  uint64_t linked,
                                  uint64_t* runtime);
bool LoaderRuntimeAddressToLinked(Loader* loader,
                                  uint64_t runtime,
                                  uint64_t* linked);

// Locate a linked ELF preinit/init/fini function-pointer array in the main
// executable and translate its start address for host-side inspection.
bool LoaderGetFunctionArray(Loader* loader, int32_t section_type,
                            uint64_t* runtime_start, size_t* entry_count,
                            size_t* entry_size);
bool LoaderReadFunctionArrayEntry(uint64_t runtime_start, size_t entry_index,
                                  size_t entry_size, uint64_t* function);

// Allocate an independent TLS block initialized from the immutable PT_TLS
// template.  The caller must free() the returned pointer.
bool LoaderAllocThreadTlsBlock(const Loader* loader, void** block_out,
                               size_t* block_size_out, uint64_t* fs_base_out);

#endif /* loader_h */
