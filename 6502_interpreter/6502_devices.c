//
//  6502_devices.c
//  6502_interpreter
//
//  Created by David Allison on 5/31/21.
//  Copyright © 2021 David Allison. All rights reserved.
//

#include "6502_devices.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/poll.h>

static bool ConsoleClaim(Device* dev, uint16_t addr) {
  ACIADevice* acia = (ACIADevice*)dev;
  bool claimed = addr == acia->csr_addr || addr == acia->data_addr;
  return claimed;
}

static void ConsoleWrite(Device* dev, uint16_t addr, int v) {
  ACIADevice* acia = (ACIADevice*)dev;
  if (addr == acia->data_addr) {
    char buf[1];
    buf[0] = v & 0xff;
    write(1, buf, 1);
  } else if (addr == acia->csr_addr) {
    // Ignore for now.
  }
}

static int ConsoleRead(Device* dev, uint16_t addr) {
  ACIADevice* acia = (ACIADevice*)dev;
  if (addr == acia->data_addr) {
    char buf[1];
    read(0, buf, 1);
    return buf[0];
  }
  if (addr == acia->csr_addr) {
    int result = 2;   // TDRE
    struct pollfd fd = {0, POLLIN, 0};    // Poll for stdin.
    if (poll(&fd, 1, 0) == 1) {
      result |= 1;  // Can read.
    }
    return result;
  }
  return 0;
}

Device* ConsoleInit() {
  ACIADevice* acia = malloc(sizeof(ACIADevice));
  acia->csr_addr = 0xfe00;
  acia->data_addr = 0xfe01;
  acia->device.claim = ConsoleClaim;
  acia->device.write = ConsoleWrite;
  acia->device.read = ConsoleRead;
  return &acia->device;
}
