//
//  dynamic.c
//  common_utils
//
//  Created by David Allison on 2/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include "loader_dynamic.h"
#include "loader_arch.h"
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "loader.h"
#include <inttypes.h>

// MAP_ANON seems to have an issue on Raspbian.
#ifndef MAP_ANON
#define MAP_ANON 0x20
#endif

bool print_libraries_only;

void DynamicLibraryRegistryInit(DynamicLibraryRegistry* reg) {
  MapInitForStringKeys(&reg->loaded_libraries);
  VectorInit(&reg->search);
}

void DynamicLibraryRegistryDestruct(DynamicLibraryRegistry* reg) {
  VectorDestructWithContents(&reg->search,
                             (VectorElementDestructor)LoadedDynamicLibraryDestruct, /*free_element=*/true);
  MapDestruct(&reg->loaded_libraries);
}


// Align the given value to a power of 2 alignment.
static uint64_t AlignUp(uint64_t v, uint64_t alignment) {
  return (v + (alignment - 1)) & ~(alignment - 1);
}

static uint64_t AlignDown(uint64_t v, uint64_t alignment) {
  return v & ~(alignment - 1);
}

static uint64_t NextAddress(LoadedDynamicLibrary* lib, uint64_t current_address,
                            uint64_t* next_address) {
  if (lib->header->type == ET(dyn) && next_address != NULL) {
    return AlignUp(*next_address, sysconf(_SC_PAGESIZE));
  }
  return current_address;
}

static MappedSegment* NewMappedSegment(void* addr, size_t length) {
  MappedSegment* seg = malloc(sizeof(MappedSegment));
  seg->address = addr;
  seg->length = length;
  return seg;
}

static void MappedSegmentDestruct(MappedSegment* s) {
  munmap(s->address, s->length);
}

static MappedSegment* FindMappedSegmentContaining(LoadedDynamicLibrary* lib,
                                                  uint64_t address,
                                                  uint64_t length) {
  uint64_t end = address + length;
  for (size_t i = 0; i < lib->mapped_segments.length; i++) {
    MappedSegment* mapped = lib->mapped_segments.value.p[i];
    uint64_t mapped_start = (uint64_t)mapped->address;
    uint64_t mapped_end = mapped_start + mapped->length;
    if (address >= mapped_start && end <= mapped_end) {
      return mapped;
    }
  }
  return NULL;
}

// Given the name of a dynamic libary, find it in the dynamic linker's
// library map.
LoadedDynamicLibrary* DynamicLoaderFindLibrary(DynamicLibraryRegistry* registry,
                                               String* filename) {
  return MapFindPointerKey(&registry->loaded_libraries, filename);
}

bool DynamicLoaderFindSymbol(DynamicLibraryRegistry* registry,
                                             const char* name,
                             const ELFSymbol** symbol,
                             LoadedDynamicLibrary** library) {
  for (size_t i = 0; i < registry->search.length; i++) {
    LoadedDynamicLibrary* lib = registry->search.value.p[i];
    const ELFSymbol* sym = LoadedDynamicLibraryFindSymbol(lib, name);
    if (sym != NULL) {
      *symbol = sym;
      *library = lib;
      return true;
    }
  }
  return false;
}

bool DynamicLoaderLookupSymbolByAddress(DynamicLibraryRegistry* registry,
                                        uint64_t address,
                                        const char** name,
                                        uint64_t* start,
                                        uint64_t* length) {
  for (size_t i = 0; i < registry->search.length; i++) {
    LoadedDynamicLibrary* lib = registry->search.value.p[i];
    bool found = LoadedDynamicLibraryLookupSymbolByAddress(lib,
                                                           address,
                                                           name,
                                                           start,
                                                           length);
    if (found) {
      return true;
    }
  }
  return false;
}

void DynamicLibraryRegistryInsert(DynamicLibraryRegistry* reg,
                                  LoadedDynamicLibrary* lib) {
  MapKeyValue kv;
  kv.key.p = &lib->filename;
  kv.value.p = lib;
  MapInsert(&reg->loaded_libraries, kv);
  
  // TODO: use the symbolic flag to insert at the beginning
  VectorAppend(&reg->search, lib);

}

static const void* ResolveDynamicTagPointer(const LoadedDynamicLibrary* lib,
                                            uint64_t tag_value) {
  if (lib->dynamic_section_relocated) {
    return (const void*)tag_value;
  }
  if (lib->header != NULL && lib->addr != NULL) {
    if (lib->header->type == ET(dyn)) {
      for (int i = 0; i < lib->header->phnum; i++) {
        const ELFProgramHeader* ph = &lib->program_headers[i];
        if (ph->type != PT(load)) {
          continue;
        }
        if (tag_value >= ph->vaddr && tag_value < ph->vaddr + ph->memsz) {
          return (const char*)lib->addr + ph->offset + (tag_value - ph->vaddr);
        }
      }
    }
    if (lib->section_headers != NULL) {
      for (int i = 0; i < lib->header->shnum; i++) {
        const ELFSectionHeader* sh = &lib->section_headers[i];
        if (sh->type == SHT(nobits) || sh->size == 0) {
          continue;
        }
        if (tag_value >= sh->addr && tag_value < sh->addr + sh->size) {
          return (const char*)lib->addr + sh->offset + (tag_value - sh->addr);
        }
      }
    }
  }
  return (const void*)((uintptr_t)lib->load_address + tag_value);
}

const void* DynamicLoaderFindDynamicSectionAddressEntry(
                                                  const LoadedDynamicLibrary* lib,
                                                  ELFDynamicTag tag) {
  const DynamicSection* dynamic = lib->dynamic;
  for (size_t i = 0; dynamic->entries[i].tag != DT(null); i++) {
    if (dynamic->entries[i].tag == tag) {
      if (lib->dynamic_section_relocated) {
        return (const void*)dynamic->entries[i].un.val;
      }
      if (lib->loader != NULL && lib->loader->arch->ignore_vaddr) {
        uint64_t runtime = 0;
        if (!LoaderLinkedAddressToRuntime(lib->loader, lib,
                                          dynamic->entries[i].un.val,
                                          &runtime)) {
          return NULL;
        }
        return (const void*)runtime;
      }
      return ResolveDynamicTagPointer(lib, dynamic->entries[i].un.val);
    }
  }
  return NULL;
}

int64_t DynamicLoaderFindDynamicSectionOffsetEntry(
                                                        const LoadedDynamicLibrary* lib,
                                                        ELFDynamicTag tag) {
  const DynamicSection* dynamic = lib->dynamic;
  for (size_t i = 0; dynamic->entries[i].tag != DT(null); i++) {
    if (dynamic->entries[i].tag == tag) {
      return dynamic->entries[i].un.val;
    }
  }
  return -1;
}

// Calculate GNU hash code for a symbol name.  This isn't documented
// anywhere except articles on the internet and the source code.
// See the article https://flapenguin.me/2017/05/10/elf-lookup-dt-gnu-hash/
// for a good explanation.
// Another good paper is https://www.akkadia.org/drepper/dsohowto.pdf
uint32_t DynamicLoaderGNUHash(const char* name) {
  uint32_t hash = 5381;
  while (*name != '\0') {
    hash = (hash << 5) + hash + *name++;
  }
  return hash;
}

// Set the bits corresponding to bits 5:0 and bits 31:26 of the hash value.
uint64_t DynamicLoaderBloomBits64(uint32_t hash) {
  return (1LL << (hash % 64)) |
    (1LL << ((hash >> 26) % 64));
}

// Find a dynamic library file by searching for it in the given
// paths.
// If the file is absolute (rooted), it looks for it as a file.
// This looks in the runtime search paths first (also called the RPATH)
// Then it looks in the static paths (LD_LIBRARY_PATH)
static bool FindDynamicLibraryFile(Vector* static_search_path,
                                   Vector* runtime_search_path,
                                   const char* filename,
                                   String* pathname) {
  struct stat st;
  if (stat(filename, &st) == 0) {
    StringSet(pathname, filename);
    return true;
  }
  // Check as full path (legacy check for paths that failed stat above).
  if (filename[0] == '/' && stat(filename, &st) == 0) {
    StringSet(pathname, filename);
    return true;
  }
  
  // The search uses the environment variable LD_LIBRARY_PATH first
  // then the runtime_search_paths, which is set from DT_RUNPATH
  // in the dynamic section.
  
  // Search for the file in the static search path.
  if (static_search_path != NULL) {
    for (size_t i = 0; i < static_search_path->length; i++) {
      String* path = static_search_path->value.p[i];
      StringClear(pathname);
      StringPrintf(pathname, "%s/%s", path->value, filename);
      if (stat(pathname->value, &st) == 0) {
        // Found file at the path.
        return true;
      }
    }
  }
  
  // Search runtime path if is is present.
  if (runtime_search_path != NULL) {
    for (size_t i = 0; i < runtime_search_path->length; i++) {
      String* path = runtime_search_path->value.p[i];
      StringClear(pathname);
      StringPrintf(pathname, "%s/%s", path->value, filename);
      if (stat(pathname->value, &st) == 0) {
        // Found file at the path.
        return true;
      }
    }
  }
  
  return false;
}

// Load the DT_NEEDED libraries into the dynamic linker.  These
// are not added to the DT_NEEDED list of the output.
static void LoadNeededLibraries(DynamicLibraryRegistry* registry,
                                LoadedDynamicLibrary* lib,
                                Vector* search_path,
                                uint64_t load_addr,
                                uint64_t* next_available_address) {
  const DynamicSection* section = lib->dynamic;
  const char* strtab = DynamicLoaderFindDynamicSectionAddressEntry(lib, DT(strtab));
  if (strtab == NULL) {
    return;
  }
  String libname = {0};
  
  // All new libraries.  Contains pointers to LoadedDynamicLibrary but does
  // now own them.
  Vector new_libraries = {0};
  
  Vector all_search_paths = {0};
  VectorCopy(&all_search_paths, search_path);
  VectorAppendVector(&all_search_paths, &lib->runtime_search_path);
  
  // Go through all the NEEDED libraries and see if they are already loaded.
  // If they aren't create the LoadedDynamicLibrary and add it to the
  // registry.
  for (size_t i = 0; section->entries[i].tag != DT(null); i++) {
    if (section->entries[i].tag == DT(needed)) {
      const char* libname = strtab + section->entries[i].un.val;
      String pathname = {0};
      bool found = FindDynamicLibraryFile(
          lib->loader != NULL ? &lib->loader->library_search_path : search_path,
          &all_search_paths, libname, &pathname);
      if (found) {
        LoadedDynamicLibrary* dep = DynamicLoaderFindLibrary(registry, &pathname);
        if (dep == NULL) {
          LoadedDynamicLibrary* new_lib = NewLoadedDynamicLibrary(pathname.value,
                                                                lib->loader);
          DynamicLibraryRegistryInsert(registry, new_lib);
          VectorAppend(&new_libraries, new_lib);
        }
      } else {
        LoaderError("Cannot find library %s", libname);
      }
      StringDestruct(&pathname);
    }
  }
  
  // Now load all the newly created libraries - the ones that are not
  // already loaded.
  for (size_t i = 0; i < new_libraries.length; i++) {
    LoadedDynamicLibrary* lib = new_libraries.value.p[i];
    LoadedDynamicLibraryLoad(lib, registry, search_path, load_addr, next_available_address);
    load_addr = NextAddress(lib, load_addr, next_available_address);
  }
  
  StringDestruct(&libname);
  VectorDestruct(&new_libraries);
  VectorDestruct(&all_search_paths);
}



// Add the load address to all pointer values in the dynamic section.
static void RelocateDynamicSection(LoadedDynamicLibrary* lib) {
  if (lib->header->type != ET(dyn)) {
    // Only relocate if we are a dynamic library.
    lib->dynamic_section_relocated = true;
    return;
  }
  DynamicSection* section = (DynamicSection*)lib->dynamic;
  if (section == NULL) {
    return;
  }
  for (size_t i = 0; section->entries[i].tag != DT(null); i++) {
    switch (section->entries[i].tag) {
      // Every tag the loader goes on to read as an address has to be here:
      // dynamic_section_relocated then tells the accessor that the whole
      // section holds runtime addresses, so a tag left out of this list is
      // handed back with its link-time value and used as a pointer.  The
      // lifecycle arrays were missing, which pointed a shared object's
      // constructors at unmapped memory.  The ELF32 path in
      // DecodeELF32DynamicTables translates the same set.
      case DT(symtab):
      case DT(strtab):
      case DT(rel):
      case DT(rela):
      case DT(jmprel):
      case DT(pltgot):
      case DT(gnu_hash):
      case DT(preinit_array):
      case DT(init_array):
      case DT(fini_array):
        section->entries[i].un.val += lib->load_address;
        break;
      default:
        break;
    }
  }
  lib->dynamic_section_relocated = true;
}

static char* RelocationTargetAddress(LoadedDynamicLibrary* lib,
                                     uint64_t linked_offset) {
  if (lib->loader != NULL && lib->loader->arch->ignore_vaddr) {
    uint64_t runtime = 0;
    if (!LoaderLinkedAddressToRuntime(lib->loader, lib, linked_offset, &runtime)) {
      LoaderError("Cannot translate relocation target 0x%" PRIx64,
                  linked_offset);
      return NULL;
    }
    return (char*)runtime;
  }
  return (char*)lib->load_address + linked_offset;
}

static void PerformDynamicRelocations(Loader* loader, LoadedDynamicLibrary* lib,
                                      DynamicLibraryRegistry* loaded_libraries,
                                      bool lazy) {
  // All the information about how to perform dynamic relocations is
  // held in the .dynamic section, which is mapped in the DYNAMIC
  // segment.
  const DynamicSection* section = lib->dynamic;
  if (section == NULL) {
    return;
  }
  // Symbol and string tables;
  const ELFSymbol* symtab = lib->dynsym;
  const char* strtab = lib->dynstr;
  
  // Data relocations in GOT.
  ELFRelocation* data_relocations = NULL;
  
  // PLT relocations in GOT.
  ELFRelocation* plt_relocations = NULL;
  
  int64_t reloc_type = DT(rela);
  int64_t relocation_size = 0;
  int64_t relocation_entsize = 0;
  int64_t relative_count = 0;
  int64_t plt_rel_size = 0;
  for (size_t i = 0; section->entries[i].tag != DT(null); i++) {
    switch (section->entries[i].tag) {
      case DT(rela):
        data_relocations = (ELFRelocation*)
            DynamicLoaderFindDynamicSectionAddressEntry(lib, DT(rela));
        break;
      case DT(rel):
        data_relocations = (ELFRelocation*)
            DynamicLoaderFindDynamicSectionAddressEntry(lib, DT(rel));
        break;
      case DT(jmprel):
        plt_relocations = (ELFRelocation*)
            DynamicLoaderFindDynamicSectionAddressEntry(lib, DT(jmprel));
        break;
      case DT(pltrel):
        reloc_type = section->entries[i].un.val;
        break;
      case DT(relasz):
      case DT(relsz):
        relocation_size = section->entries[i].un.val;
        break;
      case DT(relacount):
      case DT(relcount):
        relative_count = section->entries[i].un.val;
        break;
      case DT(relaent):
      case DT(relent):
        relocation_entsize = section->entries[i].un.val;
        break;
      case DT(pltrelsz):
        plt_rel_size = section->entries[i].un.val;
        break;
    }
  }
  if (symtab == NULL || strtab == NULL) {
    return;
  }
  
  // The first n entries in the PLTGOT contain architecture specific resolver
  // data that is set by a function provided by the user of the loader.
  // NOTE: the naming here is strange.  The section is called .got.plt
  // but the dynamic entry is called DT_PLTGOT.  Someone should make up
  // their mind.
  loader->arch->init_got_plt(lib, loader->arch_data);
  
  // Data relocations refer to symbols so they need to be set to the
  // symbol address.
  //
  // All relocations are relative to the load address of the whole
  // library.
  if (data_relocations != NULL) {
    int64_t num_relocations = 0;
    if (relocation_size > 0 && relocation_entsize > 0) {
      num_relocations = relocation_size / relocation_entsize;
    } else if (relative_count > 0) {
      num_relocations = relative_count;
    }
    
    for (int64_t i = 0; i < num_relocations; i++) {
      ELFRelocation* reloc = &data_relocations[i];
      int32_t sym_index = ELF_R_SYM(reloc->info);
      const char* sym_name = strtab + symtab[sym_index].name;
      const ELFSymbol* symbol = NULL;
      LoadedDynamicLibrary* found_lib = NULL;
      DynamicLoaderFindSymbol(loaded_libraries,
                                      sym_name, &symbol,
                                      &found_lib);
      if (found_lib == NULL) {
        // Might not have a symbol (as in the case of RELATIVE relocations).
        found_lib = lib;
      }
      // The relocation target belongs to the library containing the
      // relocation; found_lib only supplies the resolved symbol's base.
      char* target_address = RelocationTargetAddress(lib, reloc->offset);
      if (target_address == NULL) {
        continue;
      }
      loader->arch->apply_got_data_relocation(found_lib, reloc,
                                              symbol, sym_name,
                                              target_address, lazy);
      
    }
  }
  
  // In this phase, PLT relocations just need to have the load address
  // added to them since they already contain the offset of the
  // PLT entry, relative to the start of the library.
  if (plt_relocations != NULL && plt_rel_size != 0) {
    int64_t entry_size = relocation_entsize;
    if (entry_size == 0) {
      entry_size = reloc_type != DT(rela)
                       ? sizeof(ELFRelocation) - sizeof(ELF_Sxword)
                       : sizeof(ELFRelocation);
    }
    int64_t num_relocations = plt_rel_size / entry_size;
    
    for (int64_t i = 0; i < num_relocations; i++) {
      ELFRelocation* reloc = &plt_relocations[i];
      char* target_address = RelocationTargetAddress(lib, reloc->offset);
      if (target_address == NULL) {
        continue;
      }
      int32_t sym_index = ELF_R_SYM(reloc->info);
      const char* sym_name = strtab + symtab[sym_index].name;
      const ELFSymbol* symbol = NULL;
      LoadedDynamicLibrary* found_lib = lib;
      if (!lazy) {
        DynamicLoaderFindSymbol(loaded_libraries,
                              sym_name, &symbol,
                              &found_lib);
      }
      loader->arch->apply_got_plt_relocation(found_lib, reloc,
                                             symbol, sym_name,
                                             target_address, lazy);
    }
    if (loader->arch->ignore_vaddr &&
        loader->arch->fixup_plt_after_load != NULL) {
      loader->arch->fixup_plt_after_load(loader, lib, plt_relocations,
                                         num_relocations, lazy);
    }
  }
}


// Map a dynamic library into memory, returning the fd used to
// access the file (or -1 if things go badly).
// Set *addr to the address of the library and
// *length to the length of the memory mapped.
static int MapDynamicLibraryHeader(String* filename,
                             const void** addr, size_t* length) {
  // Open the file.
  int fd = open(filename->value, O_RDONLY);
  if (fd < 0) {
    return fd;
  }
  
  // Read the header from the file.  This will allow us to find
  // how much memory we need to map in to find the program headers
  // and section headers.
  ELFHeader header;
  ssize_t n = read(fd, (char*)&header, sizeof(header));
  if (n != sizeof(header)) {
    close(fd);
    return -1;
  }

  // ELF32 files use a different (narrower) header layout, so the fields read
  // above are not where we expect.  They are also small (e.g. davecc's ARM
  // output), so just map the whole file: the segments are mapped separately
  // from the fd, and mapping everything makes the ELF32 headers and section
  // data (shstrtab etc.) fully available for decoding.
  if (((const unsigned char*)&header)[EI_CLASS] == ELFCLASS32) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
      close(fd);
      return -1;
    }
    *length = st.st_size;
    *addr = mmap(NULL, *length, PROT_READ, MAP_PRIVATE, fd, 0);
    if (*addr == MAP_FAILED) {
      close(fd);
      return -1;
    }
    return fd;
  }

  int page_size = (int)sysconf(_SC_PAGESIZE);
  off_t end_program_headers = header.phoff + header.phnum * header.phentsize;
  off_t end_section_headers = header.shoff + header.shnum * header.shentsize;
  off_t max_offset = end_program_headers > end_section_headers ?
          end_program_headers :
          end_section_headers;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }
  if (st.st_size > max_offset) {
    max_offset = st.st_size;
  }
  
  off_t map_size = AlignUp(max_offset,
                        page_size);
  *length = map_size;
  
  // Map enough of the file into memory to allow us to read the
  // program headers and section heders.  Let the OS choose the address to map
  // the file at.
  *addr = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd,
               0);
  
  // Now we have the file mapped into memory at address 'addr'.  We can
  // access this memory directly.  It is mapped read-only so we can't
  // write to it.
  
  if (*addr == MAP_FAILED) {
    return -1;
  }
  return fd;
}

// Map the whole dynamic library.  This is used in the linker when it's
// loading a library.  It doesn't need to map the segments at a certain
// address and only needs the dynamic symbol table.
static int MapWholeDynamicLibrary(String* filename,
                                   const void** addr, size_t* length) {
  struct stat st;
  int e = stat(filename->value, &st);
  if (e != 0) {
    return -1;
  }
  *length = st.st_size;
  
  // Open the file.
  int fd = open(filename->value, O_RDONLY);
  if (fd < 0) {
    return fd;
  }
  
  
  // Map enough of the file into memory to allow us to read the
  // program headers and section heders.  Let the OS choose the address to map
  // the file at.
  *addr = mmap(NULL, *length, PROT_READ, MAP_PRIVATE, fd,
               0);
  
  // Now we have the file mapped into memory at address 'addr'.  We can
  // access this memory directly.  It is mapped read-only so we can't
  // write to it.
  
  if (*addr == MAP_FAILED) {
    return -1;
  }
  return fd;
}

// Map in the memory for the symbol table and string table.  The address
// will be chosen by the OS.
static void MapSymbolTable(LoadedDynamicLibrary* lib,
                           const ELFSectionHeader* symtab,
                           const ELFSectionHeader* strtab) {
  int64_t start;
  int64_t end;
  if (symtab->offset < strtab->offset) {
    start = symtab->offset;
    end = strtab->offset + strtab->size;
  } else {
    start = strtab->offset;
    end = symtab->offset + symtab->size;
  }
  int64_t page_size = sysconf(_SC_PAGESIZE);
  int64_t page_mask = page_size - 1;
  int64_t delta = start & page_mask;    // From mapped address to start.
  int64_t start_offset = start & ~page_mask;
  int64_t length = AlignUp(end - start_offset, page_size);
  void* addr = mmap(NULL, length, PROT_READ, MAP_PRIVATE, lib->fd, start_offset);
  if (addr == MAP_FAILED) {
    return;
  }
  lib->symtab = (const ELFSymbol*)((char*)addr + delta + (symtab->offset - start));
  lib->strtab = (const char*)addr + delta + (strtab->offset - start);
  VectorAppend(&lib->mapped_segments, NewMappedSegment(addr, length));
}

static void FindAndLoadSymbolTable(LoadedDynamicLibrary* lib) {
  // Find the regular symbol table and string table so that we
  // can lookup symbols by address.  This is useful for printing
  // the location of instructions.
  // The only way to do this is to search the sections for the
  // SHT(symtab).
  for (int i = 0; i < lib->header->shnum; i++) {
    const ELFSectionHeader* section = &lib->section_headers[i];
    if (section->type == SHT(symtab)) {
      // Found symbol table section.  Its link field is the section
      // index of the string table.
      
      lib->num_symtab_symbols = section->size / section->entsize;
      const ELFSectionHeader* strtab = &lib->section_headers[section->link];
      MapSymbolTable(lib, section, strtab);
      break;
    }
  }
}

static bool LoadSegments(LoadedDynamicLibrary* lib,
                         uint64_t load_address,
                         uint64_t* end_of_library) {
  if (load_address != 0) {
    // Most of the file data is located in the PT_LOAD segments.  This
    // includes the symbol tables. The PT_DYNAMIC segment also needs
    // to be loaded.
    if (lib->header->type == ET(exec)) {
      uint64_t first = LoadedDynamicLibraryLoadSegments(lib, 0, end_of_library);
      if (first == 0) {
        return false;
      }
      if (lib->loader == NULL || !lib->loader->arch->ignore_vaddr) {
        // ET_EXEC dynamic addresses are already absolute.
        lib->load_address = 0;
      }
    } else if (lib->header->type == ET(dyn)){
      uint64_t first = LoadedDynamicLibraryLoadSegments(lib,
                                                        load_address,
                                                        end_of_library);
      if (first == 0) {
        return false;
      }
      if (lib->loader == NULL || !lib->loader->arch->ignore_vaddr) {
        lib->load_address = first;
      }
    } else {
      // Unknown ELF file type.
      LoadedDynamicLibraryDelete(lib);
      return false;
    }
  } else {
    // Load address is the address of the whole library.
    lib->load_address = (uint64_t)lib->addr;
  }
  return true;
}

static bool FindDynamicSection(LoadedDynamicLibrary* lib,
                               uint64_t load_address) {
  if (lib->dynamic == NULL) {
    for (int i = 0; i < lib->header->phnum; i++) {
      if (lib->program_headers[i].type == PT(dynamic)) {
        if (load_address == 0) {
          // If we are not loading into a particular address the dynamic segment
          // is loaded with the whole file.
          lib->dynamic = (DynamicSection*)((char*)lib->load_address +
                                           lib->program_headers[i].offset);
        } else {
          if (lib->header->type == ET(exec)) {
            uint64_t dynamic_address = lib->program_headers[i].vaddr;
            if (lib->loader != NULL && lib->loader->arch->ignore_vaddr) {
              if (!LoaderLinkedAddressToRuntime(lib->loader, lib,
                                                dynamic_address,
                                                &dynamic_address)) {
                LoaderError("Cannot translate dynamic section in %s\n",
                            lib->filename.value);
                return false;
              }
            }
            lib->dynamic = (DynamicSection*)dynamic_address;
          } else {
            lib->dynamic = (DynamicSection*)((char*)lib->load_address +
                                             lib->program_headers[i].offset);
          }
        }
        break;
      }
    }
  }
  
  if (lib->dynamic == NULL) {
    return false;
  }
  
  // Relocate the pointer entries in the dynamic section.  Only if we
  // can write to it.  If load_address was 0 we map read-only.
  if (load_address != 0 &&
      (lib->loader == NULL || !lib->loader->arch->ignore_vaddr)) {
    RelocateDynamicSection(lib);
  }
  
  // Find the symbol table and string table.
  lib->dynsym = DynamicLoaderFindDynamicSectionAddressEntry(lib,
                                                            DT(symtab));
  lib->dynstr = DynamicLoaderFindDynamicSectionAddressEntry(lib,
                                                            DT(strtab));
  
  // These must exist.
  if (lib->dynsym == NULL || lib->dynstr == NULL) {
    LoaderError("Failed to load dynamic library %s: no symbol table",
                lib->filename.value);
    return false;
  }
  return true;
}

// Find the GNU Hash table in the dynamic section.  It is most likely
// that it exists but it might not.
static void FindHashTable(LoadedDynamicLibrary* lib) {
  lib->gnu_hash = DynamicLoaderFindDynamicSectionAddressEntry(lib,
                                                              DT(gnu_hash));
  if (lib->gnu_hash == NULL) {
    // There is no GNU_HASH (and we don't support the old DT_HASH format)
    // so we will need to do a linear search for symbols.  But first we
    // need to figure out how many symbols there are.  Unfortunately, the
    // ELF designers seem to have forgotten to add that to the DYNAMIC
    // section so the only way to know is to look at the section header
    // for the SHT(dynsym) and divide its size by the entry size.
    for (int i = 0; i < lib->header->shnum; i++) {
      const ELFSectionHeader* section = &lib->section_headers[i];
      if (section->type == SHT(dynsym)) {
        lib->num_dynamic_symbols = section->size / section->entsize;
        break;
      }
    }
  }
}

// Expand all special path segments in the rpath string, replacing its
// value.
//
// There are special values in the path:
// * $ORIGIN or ${ORIGIN}: path where executable was found, with symbolic
//   links expanded.
// * $EXEC_ORIGIN or ${EXEC_ORIGIN}: path where executable was found, with no
//   symbolic link expansion.
// * $LIB or ${LIB}: "lib" or "lib64"
// * $PLATFORM or ${PLATFORM}: machine architecture name (e.g. x86_64).
void ExpandRuntimePath(LoadedDynamicLibrary* lib, String* rpath) {
  String result = {0};
  size_t i = 0;
  while (i < rpath->length) {
    String part = {0};
    while (i < rpath->length && rpath->value[i] != '/') {
      StringAppendChar(&part, rpath->value[i]);
      i++;
    }
    if (part.length > 0 && part.value[0] == '$') {
      if (StringEqual(&part, "$ORIGIN") || StringEqual(&part, "${ORIGIN}")) {
        StringSetString(&part, &lib->loader->resolved_origin);
      } else if (StringEqual(&part, "$EXEC_ORIGIN") ||
                 StringEqual(&part, "${EXEC_ORIGIN}")) {
        StringSetString(&part, &lib->loader->origin);
      } else if (StringEqual(&part, "$LIB") || StringEqual(&part, "${LIB}")) {
          StringSet(&part, "lib");
      } else if (StringEqual(&part, "$PLATFORM") ||
                 StringEqual(&part, "${PLATFORM}")) {
        StringSet(&part, lib->loader->arch->platform);
      }
    }
    StringAppendString(&result, &part);
    StringDestruct(&part);
    if (rpath->value[i] == '/') {
      StringAppendChar(&result, '/');
      i++;
    }
  }
  StringSetString(rpath, &result);
  StringDestruct(&result);
}

// Look for DT_RUNPATH in the dynamic section and build
// the runtime path vector from its contents.  Older libraries
// used DT_RPATH but this is not supported here.
static void BuildRuntimePaths(LoadedDynamicLibrary* lib) {
  int64_t runpath_offset = DynamicLoaderFindDynamicSectionOffsetEntry(lib,
                                                                  DT(runpath));
  if (runpath_offset == -1) {
    return;
  }
  
  // Runpath is a string in the dynstr.
  const char* runpath = lib->dynstr + runpath_offset;
  
  // The runpath is a colon-separated list of directories to search.
  const char* p = runpath;
  while (*p != '\0') {
    String* path = NewString("");
    while (*p != '\0' && *p != ':') {
      StringAppendChar(path, *p++);
    }
    ExpandRuntimePath(lib, path);
    if (path->length > 0) {
      VectorAppend(&lib->runtime_search_path, path);
    } else {
      StringDelete(path);
    }
    if (*p == '\0') {
      break;
    }
    p++;
  }
}

// Decode the ELF32 file header, program headers and section headers into the
// canonical (wide) in-memory structures the rest of the loader expects.  These
// are heap allocated and owned by the library (freed on destruct).  The whole
// file must already be mapped at lib->addr.
static void DecodeELF32Headers(LoadedDynamicLibrary* lib) {
  const char* base = (const char*)lib->addr;
  const ELF32Header* hdr32 = (const ELF32Header*)base;

  ELFHeader* header = calloc(1, sizeof(ELFHeader));
  memcpy(header->ident, hdr32->ident, sizeof(header->ident));
  header->type = hdr32->type;
  header->machine = hdr32->machine;
  header->version = hdr32->version;
  header->entry = hdr32->entry;
  header->phoff = hdr32->phoff;
  header->shoff = hdr32->shoff;
  header->flags = hdr32->flags;
  header->ehsize = hdr32->ehsize;
  header->phentsize = hdr32->phentsize;
  header->phnum = hdr32->phnum;
  header->shentsize = hdr32->shentsize;
  header->shnum = hdr32->shnum;
  header->shstrndx = hdr32->shstrndx;
  lib->header = header;
  lib->owns_decoded = true;

  ELFProgramHeader* phdrs = calloc(header->phnum > 0 ? header->phnum : 1,
                                   sizeof(ELFProgramHeader));
  for (int i = 0; i < header->phnum; i++) {
    const ELF32ProgramHeader* in =
        (const ELF32ProgramHeader*)(base + header->phoff +
                                    (size_t)i * sizeof(ELF32ProgramHeader));
    phdrs[i].type = in->type;
    phdrs[i].flags = in->flags;
    phdrs[i].offset = in->offset;
    phdrs[i].vaddr = in->vaddr;
    phdrs[i].paddr = in->paddr;
    phdrs[i].filesz = in->filesz;
    phdrs[i].memsz = in->memsz;
    phdrs[i].align = in->align;
  }
  lib->program_headers = phdrs;

  ELFSectionHeader* shdrs = calloc(header->shnum > 0 ? header->shnum : 1,
                                   sizeof(ELFSectionHeader));
  for (int i = 0; i < header->shnum; i++) {
    const ELF32SectionHeader* in =
        (const ELF32SectionHeader*)(base + header->shoff +
                                    (size_t)i * sizeof(ELF32SectionHeader));
    shdrs[i].name = in->name;
    shdrs[i].type = in->type;
    shdrs[i].flags = in->flags;
    shdrs[i].addr = in->addr;
    shdrs[i].offset = in->offset;
    shdrs[i].size = in->size;
    shdrs[i].link = in->link;
    shdrs[i].info = in->info;
    shdrs[i].addralign = in->addralign;
    shdrs[i].entsize = in->entsize;
  }
  lib->section_headers = shdrs;
}

static ELFDynamicSectionEntry* FindDecodedDynamicEntry(
    ELFDynamicSectionEntry* entries, size_t count, ELFDynamicTag tag) {
  for (size_t i = 0; i < count; i++) {
    if (entries[i].tag == tag) {
      return &entries[i];
    }
  }
  return NULL;
}

static ELFRelocation* DecodeELF32RelocationSection(
    const char* base, const ELFSectionHeader* section) {
  size_t entsize = section->entsize;
  if (entsize == 0) {
    entsize = section->type == SHT(rel)
                  ? 2 * sizeof(ELF32_Word)
                  : sizeof(ELF32Relocation);
  }
  size_t count = section->size / entsize;
  ELFRelocation* relocations =
      calloc(count > 0 ? count : 1, sizeof(ELFRelocation));
  const char* contents = base + section->offset;
  for (size_t i = 0; i < count; i++) {
    const ELF32_Word* in =
        (const ELF32_Word*)(contents + i * entsize);
    relocations[i].offset = in[0];
    relocations[i].info =
        ELF64_R_INFO(ELF32_R_SYM(in[1]), ELF32_R_TYPE(in[1]));
    if (section->type == SHT(rela)) {
      relocations[i].addend =
          ((const ELF32Relocation*)(contents + i * entsize))->addend;
    }
  }
  return relocations;
}

static bool TranslateELF32DynamicPointer(LoadedDynamicLibrary* lib,
                                         ELFDynamicSectionEntry* entry) {
  if (entry->un.val == 0) {
    return true;
  }
  uint64_t runtime = 0;
  if (!LoaderLinkedAddressToRuntime(lib->loader, lib, entry->un.val,
                                    &runtime)) {
    return false;
  }
  entry->un.val = runtime;
  return true;
}

// Load an ELF32 dynamic object into a running address space (load_address != 0,
// used by the runtime loader/interpreter). Decode all narrow on-disk tables
// that the generic loader indexes as canonical wide structures.
static bool SetupELF32RuntimeLibrary(LoadedDynamicLibrary* lib,
                                     uint64_t load_address,
                                     uint64_t* end_of_library) {
  DecodeELF32Headers(lib);

  if (!LoadSegments(lib, load_address, end_of_library)) {
    return false;
  }

  const char* base = (const char*)lib->addr;
  const ELFHeader* header = lib->header;
  const ELFSectionHeader* shdrs = lib->section_headers;
  const ELFSectionHeader* dynamic_sec = NULL;
  const ELFSectionHeader* dynsym_sec = NULL;
  for (int i = 0; i < header->shnum; i++) {
    if (shdrs[i].type == SHT(dynamic)) {
      dynamic_sec = &shdrs[i];
    } else if (shdrs[i].type == SHT(dynsym)) {
      dynsym_sec = &shdrs[i];
    }
  }
  if (dynamic_sec == NULL || dynsym_sec == NULL) {
    LoaderError("Failed to load dynamic library %s: missing ELF32 tables",
                lib->filename.value);
    return false;
  }

  size_t dynamic_entsize = dynamic_sec->entsize != 0
                               ? dynamic_sec->entsize
                               : sizeof(ELF32DynamicSectionEntry);
  size_t dynamic_count = dynamic_sec->size / dynamic_entsize;
  ELFDynamicSectionEntry* entries =
      calloc(dynamic_count + 1, sizeof(ELFDynamicSectionEntry));
  for (size_t i = 0; i < dynamic_count; i++) {
    const ELF32DynamicSectionEntry* in =
        (const ELF32DynamicSectionEntry*)(base + dynamic_sec->offset +
                                          i * dynamic_entsize);
    entries[i].tag = in->tag;
    entries[i].un.val = in->un.val;
  }
  entries[dynamic_count].tag = DT(null);
  lib->dynamic = (const DynamicSection*)entries;

  size_t symbol_entsize = dynsym_sec->entsize != 0
                              ? dynsym_sec->entsize
                              : sizeof(ELF32Symbol);
  size_t symbol_count = dynsym_sec->size / symbol_entsize;
  ELFSymbol* symbols =
      calloc(symbol_count > 0 ? symbol_count : 1, sizeof(ELFSymbol));
  for (size_t i = 0; i < symbol_count; i++) {
    const ELF32Symbol* in =
        (const ELF32Symbol*)(base + dynsym_sec->offset +
                             i * symbol_entsize);
    symbols[i].name = in->name;
    symbols[i].info = in->info;
    symbols[i].other = in->other;
    symbols[i].shndx = in->shndx;
    symbols[i].value = in->value;
    symbols[i].size = in->size;
  }
  lib->dynsym = symbols;
  lib->num_dynamic_symbols = (int64_t)symbol_count;
  const ELFSectionHeader* dynstr_sec = &shdrs[dynsym_sec->link];
  lib->dynstr = base + dynstr_sec->offset;
  lib->owns_dynamic_tables = true;

  ELFDynamicSectionEntry* data_entry =
      FindDecodedDynamicEntry(entries, dynamic_count, DT(rel));
  if (data_entry == NULL) {
    data_entry = FindDecodedDynamicEntry(entries, dynamic_count, DT(rela));
  }
  ELFDynamicSectionEntry* plt_entry =
      FindDecodedDynamicEntry(entries, dynamic_count, DT(jmprel));
  const ELFSectionHeader* shstr_sec = &shdrs[header->shstrndx];
  const char* shstrtab = base + shstr_sec->offset;
  for (int i = 0; i < header->shnum; i++) {
    const ELFSectionHeader* section = &shdrs[i];
    if (section->type != SHT(rel) && section->type != SHT(rela)) {
      continue;
    }
    const char* section_name = shstrtab + section->name;
    if (data_entry != NULL &&
        (strcmp(section_name, ".rel.dyn") == 0 ||
         strcmp(section_name, ".rela.dyn") == 0)) {
      lib->decoded_data_relocations =
          DecodeELF32RelocationSection(base, section);
      data_entry->un.val =
          (ELF_Xword)(uintptr_t)lib->decoded_data_relocations;
    }
    if (plt_entry != NULL &&
        (strcmp(section_name, ".rel.plt") == 0 ||
         strcmp(section_name, ".rela.plt") == 0)) {
      lib->decoded_plt_relocations =
          DecodeELF32RelocationSection(base, section);
      plt_entry->un.val =
          (ELF_Xword)(uintptr_t)lib->decoded_plt_relocations;
    }
  }

  ELFDynamicSectionEntry* data_size_entry =
      FindDecodedDynamicEntry(entries, dynamic_count,
                              data_entry != NULL && data_entry->tag == DT(rel)
                                  ? DT(relsz)
                                  : DT(relasz));
  ELFDynamicSectionEntry* plt_size_entry =
      FindDecodedDynamicEntry(entries, dynamic_count, DT(pltrelsz));
  if (data_entry != NULL && lib->decoded_data_relocations == NULL) {
    if (data_size_entry != NULL && data_size_entry->un.val != 0) {
      LoaderError("Cannot decode ELF32 dynamic relocations in %s",
                  lib->filename.value);
      return false;
    }
    data_entry->un.val = 0;
  }
  if (plt_entry != NULL && lib->decoded_plt_relocations == NULL) {
    if (plt_size_entry != NULL && plt_size_entry->un.val != 0) {
      LoaderError("Cannot decode ELF32 PLT relocations in %s",
                  lib->filename.value);
      return false;
    }
    plt_entry->un.val = 0;
  }

  ELFDynamicSectionEntry* symtab_entry =
      FindDecodedDynamicEntry(entries, dynamic_count, DT(symtab));
  ELFDynamicSectionEntry* strtab_entry =
      FindDecodedDynamicEntry(entries, dynamic_count, DT(strtab));
  if (symtab_entry != NULL) {
    symtab_entry->un.val = (ELF_Xword)(uintptr_t)symbols;
  }
  if (strtab_entry != NULL) {
    strtab_entry->un.val = (ELF_Xword)(uintptr_t)lib->dynstr;
  }
  const ELFDynamicTag pointer_tags[] = {
      DT(pltgot), DT(preinit_array), DT(init_array), DT(fini_array),
  };
  for (size_t i = 0; i < sizeof(pointer_tags) / sizeof(pointer_tags[0]); i++) {
    ELFDynamicSectionEntry* entry =
        FindDecodedDynamicEntry(entries, dynamic_count, pointer_tags[i]);
    if (entry != NULL && !TranslateELF32DynamicPointer(lib, entry)) {
      LoaderError("Cannot translate ELF32 dynamic pointer in %s",
                  lib->filename.value);
      return false;
    }
  }

  lib->dynamic_section_relocated = true;

  // Force the slow (linear) symbol search. The ELF32 GNU hash bloom filter
  // uses 32-bit words, while the canonical loader hash code uses 64-bit words.
  lib->gnu_hash = NULL;
  return true;
}

// Decode an ELF32 dynamic library into the canonical (wide) in-memory
// structures the rest of the loader expects.  Only the read-only "map the
// whole file" path (load_address == 0), which the linker uses to discover the
// symbols a shared object exports, is supported here.  The runtime path is
// handled by SetupELF32RuntimeLibrary.
static bool SetupELF32DynamicLibrary(LoadedDynamicLibrary* lib,
                                     uint64_t load_address) {
  (void)load_address;
  DecodeELF32Headers(lib);
  const char* base = (const char*)lib->addr;
  const ELFHeader* header = lib->header;
  const ELFSectionHeader* shdrs = lib->section_headers;

  // The whole file is mapped, so the load address is its base.
  lib->load_address = (uint64_t)lib->addr;

  // Decode the dynamic section.  Respect the section's sh_entsize so that we
  // read it correctly regardless of whether the entries use the 8-byte ELF32
  // layout or the 16-byte (wide) layout that davecc currently emits even for
  // ELF32 output.
  for (int i = 0; i < header->shnum; i++) {
    if (shdrs[i].type == SHT(dynamic)) {
      size_t entsize = shdrs[i].entsize;
      if (entsize == 0) {
        entsize = sizeof(ELF32DynamicSectionEntry);
      }
      size_t count = shdrs[i].size / entsize;
      ELFDynamicSectionEntry* entries =
          calloc(count + 1, sizeof(ELFDynamicSectionEntry));
      const char* p = base + shdrs[i].offset;
      for (size_t e = 0; e < count; e++) {
        if (entsize == sizeof(ELFDynamicSectionEntry)) {
          entries[e] = *(const ELFDynamicSectionEntry*)(p + e * entsize);
        } else {
          const ELF32DynamicSectionEntry* in =
              (const ELF32DynamicSectionEntry*)(p + e * entsize);
          entries[e].tag = in->tag;
          entries[e].un.val = in->un.val;
        }
      }
      entries[count].tag = DT(null);
      lib->dynamic = (const DynamicSection*)entries;
      lib->owns_dynamic_tables = true;
      break;
    }
  }
  if (lib->dynamic == NULL) {
    LoaderError("Failed to load dynamic library %s: no dynamic section",
                lib->filename.value);
    return false;
  }

  // Find the dynamic symbol table and its string table via the section
  // headers and decode the symbols into wide structures, again respecting the
  // section's sh_entsize (16-byte ELF32 symbols or 24-byte wide symbols).
  for (int i = 0; i < header->shnum; i++) {
    if (shdrs[i].type == SHT(dynsym)) {
      const ELFSectionHeader* dynsym_sec = &shdrs[i];
      const ELFSectionHeader* dynstr_sec = &shdrs[dynsym_sec->link];
      size_t entsize = dynsym_sec->entsize;
      if (entsize == 0) {
        entsize = sizeof(ELF32Symbol);
      }
      size_t count = dynsym_sec->size / entsize;
      ELFSymbol* syms = calloc(count > 0 ? count : 1, sizeof(ELFSymbol));
      const char* p = base + dynsym_sec->offset;
      for (size_t s = 0; s < count; s++) {
        if (entsize == sizeof(ELFSymbol)) {
          syms[s] = *(const ELFSymbol*)(p + s * entsize);
        } else {
          const ELF32Symbol* in = (const ELF32Symbol*)(p + s * entsize);
          syms[s].name = in->name;
          syms[s].info = in->info;
          syms[s].other = in->other;
          syms[s].shndx = in->shndx;
          syms[s].value = in->value;
          syms[s].size = in->size;
        }
      }
      lib->dynsym = syms;
      lib->num_dynamic_symbols = (int64_t)count;
      lib->dynstr = base + dynstr_sec->offset;
      break;
    }
  }
  if (lib->dynsym == NULL || lib->dynstr == NULL) {
    LoaderError("Failed to load dynamic library %s: no dynamic symbol table",
                lib->filename.value);
    return false;
  }

  // Force the slow (linear) symbol search.  The GNU hash table's bloom filter
  // uses 32-bit words in ELF32, which the wide hash lookup does not handle.
  lib->gnu_hash = NULL;
  return true;
}

bool LoadedDynamicLibraryLoad(LoadedDynamicLibrary* lib,
                              DynamicLibraryRegistry* registry,
                              Vector* search_path,
                              uint64_t load_address,
                              uint64_t* end_of_library) {
  // Find the pathname for the file.
  if (!FindDynamicLibraryFile(search_path, &lib->runtime_search_path,
                              lib->libname.value,
                              &lib->filename)) {
    return false;
  }
  const void* addr;
  size_t length;
  
  if (load_address == 0) {
     lib->fd = MapWholeDynamicLibrary(&lib->filename, &addr, &length);
  } else {
    // Try to map the file header into memory.  This includes just
    // enough to include the program headers and section headers.
    lib->fd = MapDynamicLibraryHeader(&lib->filename, &addr, &length);
  }
  if (lib->fd < 0) {
    return false;
  }
  
  lib->addr = addr;
  lib->length = length;

  const unsigned char* ident = (const unsigned char*)lib->addr;
  if (ident[EI_CLASS] == ELFCLASS32) {
    // ELF32 files (e.g. 32-bit ARM) use narrower on-disk structures with a
    // different field order, so decode them into the canonical wide structures.
    bool ok = load_address == 0
                  ? SetupELF32DynamicLibrary(lib, load_address)
                  : SetupELF32RuntimeLibrary(lib, load_address, end_of_library);
    if (!ok) {
      return false;
    }
  } else {
    lib->header = (const ELFHeader*)lib->addr;
    lib->program_headers = (const ELFProgramHeader*)(lib->addr + lib->header->phoff);
    lib->section_headers = (const ELFSectionHeader*)(lib->addr + lib->header->shoff);

    if (!LoadSegments(lib, load_address, end_of_library)) {
      return false;
    }

    if (print_libraries_only) {
      uint64_t base_address = lib->load_address;
      if (base_address != 0) {
        printf("\t%s => %s (0x%" PRIx64 ")\n", lib->libname.value,
               lib->filename.value, base_address);
      }
    }

    // Find the PT(dynamic) segment.
    if (!FindDynamicSection(lib, load_address)) {
      return false;
    }

    // Find the symbol hash table.  It is most likely that it is
    // present.
    FindHashTable(lib);
  }
  
  // Build runtime paths from DT_RPATH or DT_RUNPATH.
  if (lib->loader != NULL) {
    BuildRuntimePaths(lib);
  }
  
  if (lib->load_symbol_table) {
    FindAndLoadSymbolTable(lib);
  }
  
  // Move to next load address.
  load_address = NextAddress(lib, load_address, end_of_library);
  
  // Load all libraries needed by this one.
  LoadNeededLibraries(registry, lib, search_path, load_address, end_of_library);
  return true;
}

LoadedDynamicLibrary* NewLoadedDynamicLibrary(const char* libname,
                                              Loader* loader) {
  // Create the dynamic library struct.
  LoadedDynamicLibrary* lib = malloc(sizeof(LoadedDynamicLibrary));
  lib->loader = loader;
  lib->fd = 0;
  StringInit(&lib->libname, libname);
  StringInit(&lib->filename, NULL);

  lib->addr = 0;
  lib->length = 0;
  lib->num_dynamic_symbols = 0;
  lib->header = NULL;
  lib->dynamic = NULL;
  lib->dynstr = NULL;
  lib->dynsym = NULL;
  lib->gnu_hash = NULL;
  lib->program_headers = NULL;
  lib->section_headers = NULL;
  lib->load_address = 0;
  VectorInit(&lib->mapped_segments);
  VectorInit(&lib->runtime_search_path);
  lib->load_symbol_table = loader == NULL ? false :
            (loader->flags & LOADER_MAP_SYMTAB) != 0;
  lib->symtab = NULL;
  lib->strtab = NULL;
  lib->num_symtab_symbols = 0;
  lib->dynamic_section_relocated = false;
  lib->owns_decoded = false;
  lib->owns_dynamic_tables = false;
  lib->decoded_data_relocations = NULL;
  lib->decoded_plt_relocations = NULL;
  return lib;
}

void LoadedDynamicLibraryDestruct(LoadedDynamicLibrary* lib) {
  if (lib->owns_decoded) {
    // For ELF32 the header, program headers and section headers were decoded
    // into heap memory (see DecodeELF32Headers).
    free((void*)lib->header);
    free((void*)lib->program_headers);
    free((void*)lib->section_headers);
  }
  if (lib->owns_dynamic_tables) {
    // The ELF32 linker path also decodes the dynamic section and dynamic
    // symbol table into heap memory.  dynstr still points into the mapped
    // file, so it is not freed here.
    free((void*)lib->dynamic);
    free((void*)lib->dynsym);
  }
  free(lib->decoded_data_relocations);
  free(lib->decoded_plt_relocations);
  if (lib->length > 0) {
    munmap((void*)lib->addr, lib->length);
  }
  close(lib->fd);
  VectorDestructWithContents(&lib->mapped_segments,
                             (VectorElementDestructor)MappedSegmentDestruct, /*free_element=*/true);
  VectorDestructWithContents(&lib->runtime_search_path,
                             (VectorElementDestructor)StringDestruct, /*free_element=*/true);
}

void LoadedDynamicLibraryDelete(LoadedDynamicLibrary* lib) {
  LoadedDynamicLibraryDestruct(lib);
  free(lib);
}

// Find a symbol by a linear search.  This is only used when there is
// no GNU_HASH section, which is very unlikely.
static const ELFSymbol* FindSymbolSlowly(LoadedDynamicLibrary* lib,
                                   const char* name) {
  for (int64_t i = 0; i < lib->num_dynamic_symbols; i++) {
    const ELFSymbol* symbol = &lib->dynsym[i];
    const char* symbol_name = &lib->dynstr[symbol->name];
    if (symbol->shndx == 0) {
      // Symbol is undefined.
      continue;
    }
    if (strcmp(symbol_name, name) == 0) {
      return symbol;
    }
  }
  return NULL;
}

// Find a symbol in a library by the fastest means possible.
const ELFSymbol* LoadedDynamicLibraryFindSymbol(LoadedDynamicLibrary* lib,
                                                    const char* name) {
  if (lib->gnu_hash == NULL) {
    // No option but to do a linear search.
    return FindSymbolSlowly(lib, name);
  }
  
  // Search the hash table.
  // First get hash value.
  uint32_t hash = DynamicLoaderGNUHash(name);
  
  // Look in Bloom filter.  If either bit is not set the symbol doesn't
  // exist.  The bloom filter is an array of 64 bit words located immediately
  // after the hash table header.
  uint64_t* bloom_filter = (uint64_t*)(lib->gnu_hash + 1);
  int index = (hash / 64) % lib->gnu_hash->bloom_size;
  uint64_t bits = DynamicLoaderBloomBits64(hash);
  if ((bloom_filter[index] & bits) != bits) {
    // Bloom filter says this hash code is definitely not in the table.
    return NULL;
  }
  
  // Symbol may or may not be in the table.  We find out by looking in the
  // chains array indexed by the bucket.
  int bucket = hash % lib->gnu_hash->num_buckets;
  uint32_t* buckets = (uint32_t*)(bloom_filter + lib->gnu_hash->bloom_size);
  int symbol_index = buckets[bucket];
  if (symbol_index < lib->gnu_hash->symoffset) {
    // An undefined symbol.
    return NULL;
  }
  
  // The chains are immediately after the buckets.
  uint32_t* chains = buckets + lib->gnu_hash->num_buckets;
  
  // Go through the chain entries starting at the symbol index minus
  // the symoffset from the header.  Compare the hash codes and if they
  // match perform a string compare with the symbol name.
  for (;;) {
    const ELFSymbol* symbol = &lib->dynsym[symbol_index];
    const char* symbol_name = &lib->dynstr[symbol->name];
    
    // Look at the chain entry and compare the top 31 bits of that entry
    // with the top 31 bits of the hash.  If they match do a string
    // comparison with the symbol name.
    uint32_t chain_hash = chains[symbol_index - lib->gnu_hash->symoffset];
    if ((chain_hash | 1) == (hash | 1) && strcmp(name, symbol_name) == 0) {
      if (symbol->shndx == 0) {
        return NULL;
      }
      return symbol;
    }
    
    // End of chain has the bottom bit set.
    if ((chain_hash & 1) != 0) {
      break;
    }
    symbol_index++;
  }
  return NULL;
}

// Lookup a symbol in the library by address.  
bool LoadedDynamicLibraryLookupSymbolByAddress(LoadedDynamicLibrary* lib,
                                               uint64_t address,
                                               const char** name,
                                               uint64_t* start,
                                               uint64_t* length) {
  if (lib->symtab == NULL) {
    return false;
  }
  // First make sure the address is within a loaded segment.
  bool possibly_in_this_library = false;
  for (size_t i = 0; i < lib->mapped_segments.length; i++) {
    MappedSegment* segment = lib->mapped_segments.value.p[i];
    uint64_t segment_address = (uint64_t)segment->address;
    if (address >= segment_address &&
        address < (segment_address + segment->length)) {
      possibly_in_this_library = true;
      break;
    }
  }
  if (!possibly_in_this_library) {
    return false;
  }
  
  // The symbol might be in this library, the only way to know is to
  // perform a linear search of the symbol table.  The symbol table
  // is not guaranteed to be ordered in any way and it would use too
  // much memory to load it into a hash table.  This is only used
  // for debugging prints anyway so speed isn't really that
  // important.  A debugger would load them into a hash table.
  for (int i = 0; i < lib->num_symtab_symbols; i++) {
    const ELFSymbol* symbol =  &lib->symtab[i];
    uint64_t symbol_start_address = lib->load_address + symbol->value;
    uint64_t symbol_end_address = symbol_start_address + symbol->size;
    if (address >= symbol_start_address && address < symbol_end_address) {
      *start = symbol_start_address;
      *length = symbol->size;
      *name = lib->strtab + symbol->name;
      return true;
    }
  }
  return false;
}

static bool MapDynamicIgnoreVaddrSegment(LoadedDynamicLibrary* lib,
                                         const ELFProgramHeader* segment,
                                         int64_t offset_diff,
                                         uint64_t map_length,
                                         void** out_ptr) {
  void* segment_ptr = mmap(NULL, map_length, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
  if (segment_ptr == MAP_FAILED) {
    LoaderError("Failed to map dynamic segment: %s", strerror(errno));
    return false;
  }
  if (segment->filesz > 0) {
    ssize_t bytes = pread(lib->fd, (char*)segment_ptr + offset_diff,
                          segment->filesz, (off_t)segment->offset);
    if (bytes != (ssize_t)segment->filesz) {
      LoaderError("Failed to read dynamic segment: %s", strerror(errno));
      munmap(segment_ptr, map_length);
      return false;
    }
  }
  *out_ptr = segment_ptr;
  return true;
}

static ELFProgramHeader* ProgramHeaderForSection(
    LoadedDynamicLibrary* lib, const ELFSectionHeader* section) {
  for (int i = 0; i < lib->header->phnum; i++) {
    ELFProgramHeader* segment = (ELFProgramHeader*)&lib->program_headers[i];
    if (segment->type != PT(load)) {
      continue;
    }
    uint64_t section_end = section->offset + section->size;
    uint64_t segment_end = segment->offset + segment->filesz;
    if (section->offset >= segment->offset && section_end <= segment_end) {
      return segment;
    }
  }
  return NULL;
}

static bool MapFixedAddressSection(LoadedDynamicLibrary* lib,
                                   const ELFSectionHeader* section,
                                   uint64_t runtime_address) {
  int page_size = (int)sysconf(_SC_PAGESIZE);
  uint64_t page_start = AlignDown(runtime_address, page_size);
  uint64_t page_end = AlignUp(runtime_address + section->size, page_size);
  ELFProgramHeader* segment = ProgramHeaderForSection(lib, section);

  for (uint64_t page = page_start; page < page_end; page += page_size) {
    if (FindMappedSegmentContaining(lib, page, (uint64_t)page_size) != NULL) {
      continue;
    }
    void* mapped =
        mmap((void*)page, (size_t)page_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED) {
      LoaderError("Failed to map dynamic section at address %p: %s\n",
                  (void*)page, strerror(errno));
      return false;
    }
    VectorAppend(&lib->mapped_segments,
                 NewMappedSegment(mapped, (size_t)page_size));
    if (lib->loader != NULL) {
      VectorAppend(&lib->loader->regions,
                   NewRegion(mapped, (int64_t)section->offset, page_size,
                             segment, lib));
    }
  }

  if (section->type == SHT(nobits)) {
    memset((void*)runtime_address, 0, (size_t)section->size);
  } else if (section->size > 0) {
    ssize_t bytes = pread(lib->fd, (void*)runtime_address,
                          (size_t)section->size, (off_t)section->offset);
    if (bytes != (ssize_t)section->size) {
      LoaderError("Failed to read dynamic section: %s\n", strerror(errno));
      return false;
    }
  }
  return true;
}

// Map the malloc heap that lives above '_end'.  The static loader gets this by
// enlarging the highest writable segment's memsz before mapping it, but here
// the sections are mapped one at a time and there is no segment mapping to
// extend, so the reserve needs a mapping of its own.  'writable_end' is '_end',
// whose own page the sections already cover, so the reserve starts at the page
// above it.
static bool ReserveFixedAddressHeap(LoadedDynamicLibrary* lib,
                                    uint64_t writable_end,
                                    uint64_t* next_available_address) {
  int page_size = (int)sysconf(_SC_PAGESIZE);
  uint64_t reserve_start = AlignUp(writable_end, page_size);
  // The sections are already mapped, so a MAP_FIXED reserve reaching one of
  // them would replace it with fresh zeroed pages.  A section above '_end'
  // would break the heap's contiguity anyway, so stop short of it and let the
  // first allocation that runs out of room report the failure.
  uint64_t reserve_end = reserve_start + LOADER_HEAP_RESERVE;
  for (uint64_t page = reserve_start; page < reserve_end; page += page_size) {
    if (FindMappedSegmentContaining(lib, page, (uint64_t)page_size) != NULL) {
      reserve_end = page;
      break;
    }
  }
  if (reserve_end == reserve_start) {
    return true;
  }

  size_t length = (size_t)(reserve_end - reserve_start);
  void* mapped = mmap((void*)reserve_start, length, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);
  if (mapped == MAP_FAILED) {
    LoaderError("Failed to reserve heap at address %p: %s\n",
                (void*)reserve_start, strerror(errno));
    return false;
  }
  VectorAppend(&lib->mapped_segments, NewMappedSegment(mapped, length));
  // Keep the libraries loaded after this one out of the heap.
  if (reserve_end > *next_available_address) {
    *next_available_address = reserve_end;
  }
  return true;
}

static uint64_t LoadFixedAddressSections(
    LoadedDynamicLibrary* lib, uint64_t load_address,
    uint64_t* next_available_address) {
  uint64_t first_address = 0;
  uint64_t writable_end = 0;
  *next_available_address = 0;

  for (int i = 0; i < lib->header->shnum; i++) {
    const ELFSectionHeader* section = &lib->section_headers[i];
    if ((section->flags & SHF(alloc)) == 0 || section->addr == 0 ||
        section->size == 0) {
      continue;
    }
    uint64_t runtime_address =
        (lib->header->type == ET(dyn) ? load_address : 0) + section->addr;
    if (!MapFixedAddressSection(lib, section, runtime_address)) {
      return 0;
    }
    uint64_t page_start =
        AlignDown(runtime_address, (int)sysconf(_SC_PAGESIZE));
    if (first_address == 0 || page_start < first_address) {
      first_address = page_start;
    }
    uint64_t section_end = runtime_address + section->size;
    if (section_end > *next_available_address) {
      *next_available_address = section_end;
    }
    if ((section->flags & SHF(write)) != 0 && section_end > writable_end) {
      writable_end = section_end;
    }
  }

  // Only the program owns a heap: malloc uses the executable's '_end', and
  // reserving above every library as well would carve holes out of the address
  // space the remaining libraries are loaded into.
  if (writable_end != 0 && lib->loader != NULL &&
      lib->loader->dynamic_lib == lib &&
      !ReserveFixedAddressHeap(lib, writable_end, next_available_address)) {
    return 0;
  }

  for (int i = 0; i < lib->header->phnum; i++) {
    const ELFProgramHeader* segment = &lib->program_headers[i];
    if (segment->type == PT(dynamic)) {
      uint64_t runtime_address =
          (lib->header->type == ET(dyn) ? load_address : 0) + segment->vaddr;
      lib->dynamic = (const DynamicSection*)runtime_address;
      break;
    }
  }
  return first_address;
}

// Load a loadable segment.  The load_address is where we want to load
// a dynamic segment at.  However this might need adjusted to
// account for segment and page alignments.  Returns the address
// at which the segment is loaded and sets *length to the segment length;
static void* LoadLoadableSegment(LoadedDynamicLibrary* lib,
                                    const ELFProgramHeader* segment,
                                    uint64_t load_address,
                                    uint64_t* length,
                                    void** start_address) {
  int page_size = (int)sysconf(_SC_PAGESIZE);
  uint64_t addr = lib->header->type == ET(dyn) ?
          load_address + segment->vaddr :
          segment->vaddr;
     
  // Virtual address for start of segment.
  uint64_t vaddr = addr;
  if (start_address != NULL) {
    *start_address = (void*)vaddr;
  }
 
  // The address to map at is the current load_address aligned down to
  // the boundary specified in the program header.
  addr = AlignDown(addr, segment->align);

  uint64_t offset = segment->offset;    // Offset into file.
  
  // Align address and offset to lower page boundary.  The address and
  // file offset must be page-aligned for the mmap function to operate
  // correctly (it will error out if this is not the case).
  addr = AlignDown(addr, page_size);
  offset = AlignDown(offset, page_size);
 
  // Front porch is the difference between vaddr and aligned address.
  uint64_t front_porch = vaddr - (uint64_t)addr;
  
  // Length of segment in memory.  We can map memory up the next
  // page boundary beyond the end of the file.  If the memsz is
  // beyond that we need to mmap a new ANON segment for it.
  uint64_t full_length = front_porch + segment->filesz;
  uint64_t aligned_length = AlignUp(full_length, page_size);
  *length = aligned_length;
  // uint64_t back_porch = aligned_length - full_length;
  
  if (*length == 0) {
    return NULL;
  }

  if (lib->loader != NULL && lib->loader->arch->ignore_vaddr) {
    int64_t offset_diff = (int64_t)segment->offset - (int64_t)offset;
    uint64_t map_length = AlignUp((uint64_t)offset_diff + segment->memsz,
                                  page_size);
    *length = map_length;
    void* segment_ptr = NULL;
    if (!MapDynamicIgnoreVaddrSegment(lib, segment, offset_diff, map_length,
                                      &segment_ptr)) {
      return NULL;
    }
    if (segment->memsz > segment->filesz) {
      memset((char*)segment_ptr + offset_diff + segment->filesz, 0,
             (size_t)(segment->memsz - segment->filesz));
    }
    if (lib->load_address == 0) {
      lib->load_address = (uint64_t)segment_ptr + (uint64_t)offset_diff -
                          segment->vaddr;
    }
    if (start_address != NULL) {
      *start_address = (char*)segment_ptr + offset_diff;
    }
    VectorAppend(&lib->loader->regions,
                 NewRegion(segment_ptr, offset, (int64_t)map_length,
                           (ELFProgramHeader*)segment, lib));
    VectorAppend(&lib->mapped_segments,
                 NewMappedSegment(segment_ptr, map_length));
    return segment_ptr;
  }

  // Fixed-vaddr targets execute with guest addresses equal to host addresses.
  // Populate an anonymous mapping instead of using a file-backed MAP_FIXED
  // mapping: DaveCC segments do not require matching vaddr/file-offset page
  // deltas, and macOS rejects fixed executable file mappings.  Keeping the
  // mapping writable also allows dynamic relocations before native runtimes
  // apply final execute permissions.
  *length = AlignUp(front_porch + segment->memsz, page_size);
  MappedSegment* containing =
      FindMappedSegmentContaining(lib, addr, *length);
  void* segment_ptr = (void*)addr;
  if (containing == NULL) {
    segment_ptr =
        mmap((void*)addr, *length, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);
    if (segment_ptr == MAP_FAILED) {
      LoaderError("Failed to map in dynamic segment at address %p: %s\n",
                  (void*)addr, strerror(errno));
      return NULL;
    }
    VectorAppend(&lib->mapped_segments,
                 NewMappedSegment(segment_ptr, *length));
    if (lib->loader != NULL) {
      VectorAppend(&lib->loader->regions,
                   NewRegion(segment_ptr, (int64_t)segment->offset,
                             (int64_t)*length, (ELFProgramHeader*)segment,
                             lib));
    }
  }
  if (segment->filesz > 0) {
    ssize_t bytes =
        pread(lib->fd, (char*)segment_ptr + front_porch, segment->filesz,
              (off_t)segment->offset);
    if (bytes != (ssize_t)segment->filesz) {
      LoaderError("Failed to read dynamic segment: %s\n", strerror(errno));
      return NULL;
    }
  }
  if (start_address != NULL) {
    *start_address = (char*)segment_ptr + front_porch;
  }
  return segment_ptr;
}


#if 0
if (segment->type == PT(dynamic)) {
  // We have mapped the dynamic segment to the page boundary just below
  // it.  We need to move the address down so that when segment->offset
  // is added to the address it will point to the correct address.
  // For example:
  // segment->offset = 0x3480
  // offset = 0x3000
  // segment_ptr = 0x60040000
  // The actual address of the dynamic section is 0x60040000 - 0x3000)
  // So that when segment->offset (0x3480) is added to it we get
  // the actual address.
  lib->dynamic = (DynamicSection*)((char*)segment_ptr -
                                   offset);
}
#endif

// Load the segments of the dynamic library starting at the given
// address.  The PT_LOAD segments are loaded in order specified
// in the file.  Returns the address of the first loaded segment.
// Sets *next_available_address to the address at the end of
// all loaded segments.
uint64_t LoadedDynamicLibraryLoadSegments(LoadedDynamicLibrary* lib,
            uint64_t load_address, uint64_t* next_available_address) {
  if (lib->loader != NULL && !lib->loader->arch->ignore_vaddr) {
    return LoadFixedAddressSections(lib, load_address,
                                    next_available_address);
  }

  *next_available_address = 0;
  uint64_t first_loaded_segment = 0;
  bool saw_load_segment = false;
  for (int i = 0; i < lib->header->phnum; i++) {
    const ELFProgramHeader* segment = &lib->program_headers[i];
    if (segment->type != PT(load) && segment->type != PT(dynamic)) {
      continue;
    }

    saw_load_segment = saw_load_segment || segment->type == PT(load);
    uint64_t length = 0;
    void* dynamic_address = NULL;
    void* addr = LoadLoadableSegment(
        lib, segment, load_address, &length,
        segment->type == PT(dynamic) ? &dynamic_address : NULL);
    if (addr == NULL) {
      return 0;
    }
    if (segment->type == PT(dynamic)) {
      lib->dynamic = (const DynamicSection*)dynamic_address;
    } else if (first_loaded_segment == 0) {
      first_loaded_segment = (uint64_t)addr;
    }
    uint64_t end_of_segment = (uint64_t)addr + length;
    if (end_of_segment > *next_available_address) {
      *next_available_address = end_of_segment;
    }
  }
  return saw_load_segment ? first_loaded_segment : 0;
}

void LoadedDynamicLibraryRelocate(Loader* loader,
                                  LoadedDynamicLibrary* lib,
                                  DynamicLibraryRegistry* registry,
                                  bool lazy) {
  PerformDynamicRelocations(loader, lib, registry, lazy);
}


