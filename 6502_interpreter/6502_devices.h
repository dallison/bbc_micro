//
//  6502_devices.h
//  6502_interpreter
//
//  Created by David Allison on 5/31/21.
//  Copyright © 2021 David Allison. All rights reserved.
//

#include <stdint.h>
#include <stdbool.h>

#ifndef _502_devices_h
#define _502_devices_h

typedef struct Device {
  bool (*claim)(struct Device* dev, uint16_t addr);
  void (*write)(struct Device* dev, uint16_t addr, int v);
  int (*read)(struct Device* dev, uint16_t addr);
} Device;

typedef struct {
  Device device;
  uint16_t csr_addr;
  uint16_t data_addr;
} ACIADevice;

Device* ConsoleInit(void);

#endif /* _502_devices_h */
