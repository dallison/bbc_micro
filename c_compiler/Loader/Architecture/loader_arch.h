//
//  loader_arch.h
//  common_utils
//
//  Created by David Allison on 3/8/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include "loader.h"
#include "loader_dynamic.h"

#ifndef loader_arch_h
#define loader_arch_h

typedef struct LoaderArchitecture {
  int machine_type;
  const char* platform;
  bool ignore_vaddr;
  size_t tls_tcb_size;
  void (*init_tls_tcb)(void* tcb, uint64_t thread_pointer);
  void (*init_got_plt)(LoadedDynamicLibrary* lib, void* data);
  void (*apply_got_data_relocation)(LoadedDynamicLibrary* lib,
                                    const ELFRelocation* reloc,
                                    const ELFSymbol* symbol,
                                    const char* sym_name,
                                    char* target_address,
                                    bool lazy);
  void (*apply_got_plt_relocation)(LoadedDynamicLibrary* lib,
                                   const ELFRelocation* reloc,
                                   const ELFSymbol* symbol,
                                   const char* sym_name,
                                   char* target_address,
                                   bool lazy);
  void (*fixup_plt_after_load)(struct Loader* loader,
                               LoadedDynamicLibrary* lib,
                               const ELFRelocation* plt_relocations,
                               int64_t num_relocations,
                               bool lazy);
} LoaderArchitecture;

#endif /* loader_arch_h */
