//
//  loader_arch_6502.h
//  p_code_interpreter
//
//  Created by David Allison on 6/25/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#ifndef loader_arch_6502_h
#define loader_arch_6502_h

#include "loader_arch.h"

void W65C02LoaderArchitectureInit(LoaderArchitecture* arch);
LoaderArchitecture* New6502LoaderArchitecture(void);

#endif /* loader_arch_6502_h */
