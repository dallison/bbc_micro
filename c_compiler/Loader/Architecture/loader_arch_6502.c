//
//  loader_arch_6502.c
//  p_code_interpreter
//
//  Created by David Allison on 6/25/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include <stdlib.h>
#include "loader_arch_6502.h"

// PIC is not supported on 6502.
static void InitGOTPLT(LoadedDynamicLibrary* lib, void* data) {
}

static void ApplyGOTDataRelocation(LoadedDynamicLibrary* lib,
                                   const ELFRelocation* reloc,
                                   const ELFSymbol* symbol,
                                   const char* sym_name,
                                   char* target_address,
                                   bool lazy) {
}

static void ApplyGOTPLTRelocation(LoadedDynamicLibrary* lib,
                                  const ELFRelocation* reloc,
                                  const ELFSymbol* symbol,
                                  const char* sym_name,
                                  char* target_address,
                                  bool lazy) {
  
}

void W65C02LoaderArchitectureInit(LoaderArchitecture* arch) {
  arch->machine_type = ELF_MACHINE_TYPEW65C02;
  arch->platform = "6502";
  arch->ignore_vaddr = true;
  arch->tls_tcb_size = 0;
  arch->init_tls_tcb = NULL;
  arch->init_got_plt = InitGOTPLT;
  arch->apply_got_data_relocation = ApplyGOTDataRelocation;
  arch->apply_got_plt_relocation = ApplyGOTPLTRelocation;
  arch->fixup_plt_after_load = NULL;
}

LoaderArchitecture* New6502LoaderArchitecture(void) {
  LoaderArchitecture* arch = malloc(sizeof(LoaderArchitecture));
  W65C02LoaderArchitectureInit(arch);
  return arch;
}
