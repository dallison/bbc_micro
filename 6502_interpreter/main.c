//
//  main.c
//  6502_interpreter
//
//  Created by David Allison on 5/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include <stdio.h>
#include "6502_interpreter.h"
#include "bbc_hardware.h"
#include "cassette.h"
#include "cumana.h"
#include "joystick.h"
#include <stdlib.h>
#include <string.h>

static void Usage() {
  fprintf(stderr,
          "usage: 6502 [-debug] [-trace] [-cycle]\n"
          "           [-bbc-machine b|master|master128|master256]\n"
          "           [-bbc-sram spec] [-bbc-mode 0-7] [-bbc-screen file.ppm]\n"
          "           [-bbc-os file] [-bbc-rom slot,file] [-bbc-roms dir]\n"
          "           [-bbc-disc file] [-bbc-disc-port n] [-bbc-hd file] [-bbc-fdc 8271|1770]\n"
          "           [-bbc-fs dfs|adfs] [-bbc-keyboard uk|us]\n"
          "           [-bbc-65c02] [-bbc-tape file] [-bbc-tape-port n]\n"
          "           [-bbc-printer file] [-bbc-2mhz] [-bbc-mhz n]\n"
          "           [-bbc-econet station] [-bbc-econet-port n]\n"
          "           [-bbc-shift] [-bbc-steps n] [-bbc-ram file]\n"
          "       Model B ROMs are read from bbc_b_rom_sockets and\n"
          "       Master ROMs from bbc_master_rom_sockets. os.rom or\n"
          "       os-<name>.rom is the OS. <socket>-<name>.rom is sideways\n"
          "       socket 0-15, virtual image 0. <socket>.<virtual>-<name>.rom\n"
          "       is image 0-7 in that socket, chosen by bits 4-6 of the ROM\n"
          "       select register. -bbc-os and -bbc-rom replace one of those files.\n"
          "       master uses a 65SC12, a WD1770, and sideways RAM in sockets\n"
          "       4-7. master256 makes every socket RAM. -bbc-sram 4, 8, 16,\n"
          "       or a list such as 4-7 replaces that set.\n"
          "       A Model B with an OS ROM is an NMOS 6502. -bbc-65c02 keeps\n"
          "       the CMOS opcodes.\n"
          "       DFS and ADFS ROMs are both fitted. The drive follows the one\n"
          "       the OS calls. -bbc-fs dfs or -bbc-fs adfs loads only that\n"
          "       filing system.\n"
          "       -bbc-keyboard uk is the BBC MODE 7 punctuation, where { and }\n"
          "       are the fraction signs. us draws the braces and brackets.\n"
          "       cumana inserts a disc while -bbc is running. It connects to\n"
          "       127.0.0.1:8177. -bbc-disc-port 0 closes that socket.\n"
          "       -bbc-hd file mounts an ADFS hard disc, a file of 256-byte\n"
          "       sectors. It is drive 0, and ADFS is the filing system that\n"
          "       starts. *CAT or *MOUNT 0 selects it.\n"
          "       cassette inserts a tape. It connects to 127.0.0.1:8178.\n"
          "       -bbc-tape-port 0 closes that socket. -bbc-tape still loads a\n"
          "       tape before the machine starts.\n"
          "       -bbc-econet n fits the Econet interface as station n (1-254).\n"
          "       Stations on the same -bbc-econet-port share a wire. The\n"
          "       default port is 8179.\n"
          "       A busy program runs ahead of the wall clock. -bbc-2mhz holds\n"
          "       the CPU at 2 MHz. -bbc-mhz n holds it at n MHz, from 1 to\n"
          "       16, with the timers and the video still in step.\n");
  exit(1);
}

int main(int argc, char *argv[]) {
  bool debug = false;
  bool cycle_accurate = false;
  bool trace = false;
  int bbc_mode = -1;
  const char* bbc_screen = NULL;
  const char* bbc_os = NULL;
  const char* bbc_roms = NULL;
  int rom_slots[16];
  const char* rom_paths[16];
  int rom_count = 0;
  const char* disc_paths[2] = {NULL, NULL};
  const char* hd_path = NULL;
  int disc_next = 0;
  int disc_port = CUMANA_PORT;
  bool disc_socket = true;
  int tape_port = CASSETTE_PORT;
  bool tape_socket = true;
  int joystick_port = JOYSTICK_PORT;
  bool joystick_socket = true;
  int fdc_kind = 0;
  int filing = BBC_FS_ANY;
  int bbc_keyboard = BBC_KEYBOARD_UK;
  bool bbc_65c02 = false;
  int bbc_mhz = 0;
  int bbc_model = BBC_MACHINE_B;
  bool bbc_sram_set = false;
  bool bbc_sram[16];
  const char* bbc_tape = NULL;
  int bbc_econet = 0;
  int bbc_econet_port = BBC_ECONET_PORT;
  const char* bbc_printer = NULL;
  bool bbc_shift = false;
  uint64_t bbc_step_limit = 0;
  const char* bbc_ram = NULL;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      if (strcmp(argv[i], "-debug") == 0) {
        debug = true;
      } else if (strcmp(argv[i], "-trace") == 0) {
          trace = true;
      } else if (strcmp(argv[i], "-cycle") == 0) {
        cycle_accurate = true;
      } else if (strcmp(argv[i], "-bbc") == 0) {
        // Accepted so an older command line still starts the machine.
      } else if (strcmp(argv[i], "-bbc-mode") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        const char* mode_text = argv[++i];
        if (mode_text[0] < '0' || mode_text[0] > '7' || mode_text[1] != '\0') {
          Usage();
        }
        bbc_mode = mode_text[0] - '0';
      } else if (strcmp(argv[i], "-bbc-screen") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_screen = argv[++i];
      } else if (strcmp(argv[i], "-bbc-os") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_os = argv[++i];
      } else if (strcmp(argv[i], "-bbc-rom") == 0) {
        if (i == argc - 1 || rom_count == 16) {
          Usage();
        }
        const char* spec = argv[++i];
        const char* comma = strchr(spec, ',');
        char slot_text[8];
        size_t slot_len;
        int slot;
        if (comma == NULL || comma == spec || comma[1] == '\0') {
          Usage();
        }
        slot_len = (size_t)(comma - spec);
        if (slot_len >= sizeof(slot_text)) {
          Usage();
        }
        memcpy(slot_text, spec, slot_len);
        slot_text[slot_len] = '\0';
        slot = atoi(slot_text);
        if (slot < 0 || slot > 15) {
          Usage();
        }
        rom_slots[rom_count] = slot;
        rom_paths[rom_count] = comma + 1;
        rom_count++;
      } else if (strcmp(argv[i], "-bbc-disc") == 0 || strcmp(argv[i], "-bbc-disc0") == 0 ||
                 strcmp(argv[i], "-bbc-disc1") == 0) {
        int drive;
        if (i == argc - 1) {
          Usage();
        }
        if (strcmp(argv[i], "-bbc-disc1") == 0) {
          drive = 1;
        } else if (strcmp(argv[i], "-bbc-disc0") == 0) {
          drive = 0;
        } else {
          drive = disc_next < 2 ? disc_next : 1;
        }
        disc_paths[drive] = argv[++i];
        if (drive >= disc_next && disc_next < 2) {
          disc_next = drive + 1;
        }
      } else if (strcmp(argv[i], "-bbc-hd") == 0 || strcmp(argv[i], "-hd") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        hd_path = argv[++i];
      } else if (strcmp(argv[i], "-bbc-disc-port") == 0) {
        char* end = NULL;
        long value;
        if (i == argc - 1) {
          Usage();
        }
        value = strtol(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value < 0 || value > 65535) {
          Usage();
        }
        if (value == 0) {
          disc_socket = false;
        } else {
          disc_port = (int)value;
        }
      } else if (strcmp(argv[i], "-bbc-tape-port") == 0) {
        char* end = NULL;
        long value;
        if (i == argc - 1) {
          Usage();
        }
        value = strtol(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value < 0 || value > 65535) {
          Usage();
        }
        if (value == 0) {
          tape_socket = false;
        } else {
          tape_port = (int)value;
        }
      } else if (strcmp(argv[i], "-bbc-joystick-port") == 0) {
        char* end = NULL;
        long value;
        if (i == argc - 1) {
          Usage();
        }
        value = strtol(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value < 0 || value > 65535) {
          Usage();
        }
        if (value == 0) {
          joystick_socket = false;
        } else {
          joystick_port = (int)value;
        }
      } else if (strcmp(argv[i], "-bbc-econet") == 0) {
        char* end = NULL;
        long value;
        if (i == argc - 1) {
          Usage();
        }
        value = strtol(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value < 1 || value > 254) {
          Usage();
        }
        bbc_econet = (int)value;
      } else if (strcmp(argv[i], "-bbc-econet-port") == 0) {
        char* end = NULL;
        long value;
        if (i == argc - 1) {
          Usage();
        }
        value = strtol(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value < 1 || value > 65535) {
          Usage();
        }
        bbc_econet_port = (int)value;
      } else if (strcmp(argv[i], "-bbc-fs") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        filing = BbcMachineParseFilingSystem(argv[++i]);
        if (filing < 0) {
          Usage();
        }
      } else if (strcmp(argv[i], "-bbc-keyboard") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_keyboard = BbcMachineParseKeyboard(argv[++i]);
        if (bbc_keyboard < 0) {
          Usage();
        }
      } else if (strcmp(argv[i], "-bbc-fdc") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        const char* which = argv[++i];
        if (strcmp(which, "8271") == 0) {
          fdc_kind = BBC_FDC_8271;
        } else if (strcmp(which, "1770") == 0) {
          fdc_kind = BBC_FDC_1770;
        } else {
          Usage();
        }
      } else if (strcmp(argv[i], "-bbc-machine") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_model = BbcMachineParseModel(argv[++i]);
        if (bbc_model < 0) {
          Usage();
        }
      } else if (strcmp(argv[i], "-bbc-sram") == 0) {
        if (i == argc - 1 || !BbcMachineParseSidewaysRam(argv[++i], bbc_sram)) {
          Usage();
        }
        bbc_sram_set = true;
      } else if (strcmp(argv[i], "-bbc-roms") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_roms = argv[++i];
      } else if (strcmp(argv[i], "-bbc-65c02") == 0) {
        bbc_65c02 = true;
      } else if (strcmp(argv[i], "-bbc-2mhz") == 0) {
        bbc_mhz = 2;
        cycle_accurate = true;
      } else if (strcmp(argv[i], "-bbc-mhz") == 0) {
        char* end = NULL;
        long value;
        if (i == argc - 1) {
          Usage();
        }
        value = strtol(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value < 1 || value > 16) {
          Usage();
        }
        bbc_mhz = (int)value;
        cycle_accurate = true;
      } else if (strcmp(argv[i], "-bbc-tape") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_tape = argv[++i];
      } else if (strcmp(argv[i], "-bbc-shift") == 0) {
        bbc_shift = true;
      } else if (strcmp(argv[i], "-bbc-steps") == 0) {
        char* end = NULL;
        unsigned long long value;
        if (i == argc - 1) {
          Usage();
        }
        value = strtoull(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value == 0) {
          Usage();
        }
        bbc_step_limit = value;
      } else if (strcmp(argv[i], "-bbc-ram") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_ram = argv[++i];
      } else if (strcmp(argv[i], "-bbc-printer") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_printer = argv[++i];
      } else {
        Usage();
      }
    } else {
      Usage();
    }
  }
  W65C02Interpreter interpreter;
  char* rom_dir = NULL;
  BbcRomFile rom_files[BBC_ROM_IMAGE_MAX];
  int rom_file_count = 0;
  bool skip_slot[16] = {false};
  {
    const char* dir = bbc_roms;
    if (dir == NULL) {
      rom_dir = BbcMachineFindRomDirectory(argv[0], BbcMachineRomDirectoryName(bbc_model));
      dir = rom_dir;
    }
    if (dir != NULL) {
      rom_file_count = BbcMachineListRomDirectory(dir, rom_files, BBC_ROM_IMAGE_MAX, filing);
      if (rom_file_count < 0) {
        free(rom_dir);
        exit(1);
      }
    }
    if (bbc_os == NULL) {
      bbc_os = BbcRomOsPath(rom_files, rom_file_count);
      if (bbc_os != NULL) {
        fprintf(stderr, "ROM os: %s\n", bbc_os);
      }
    }
  }
  W65C02InterpreterInit(&interpreter, debug, cycle_accurate, trace);
  if (bbc_mhz > 0 && !W65C02InterpreterSetClockMhz(&interpreter, bbc_mhz)) {
    Usage();
  }
  {
    int r;
    W65C02InterpreterUseBbc(&interpreter, bbc_mode, bbc_screen, bbc_os);
    if (interpreter.bbc == NULL || !BbcMachineSetModel(interpreter.bbc, bbc_model)) {
      fprintf(stderr, "Unable to select the BBC machine\n");
      exit(1);
    }
    BbcMachineSetKeyboard(interpreter.bbc, bbc_keyboard);
    if (bbc_sram_set && !BbcMachineSetSidewaysRam(interpreter.bbc, bbc_sram)) {
      fprintf(stderr, "Unable to allocate sideways RAM\n");
      exit(1);
    }
    for (r = 0; r < rom_count; r++) {
      if (!W65C02InterpreterBbcLoadRom(&interpreter, rom_slots[r], rom_paths[r])) {
        exit(1);
      }
      skip_slot[rom_slots[r]] = true;
    }
    if (!BbcMachineLoadRomFiles(interpreter.bbc, rom_files, rom_file_count, skip_slot)) {
      exit(1);
    }
    for (r = 0; r < 2; r++) {
      if (disc_paths[r] != NULL &&
          !W65C02InterpreterBbcLoadDisc(&interpreter, r, disc_paths[r])) {
        exit(1);
      }
    }
    if (hd_path != NULL && !W65C02InterpreterBbcLoadHardDisc(&interpreter, hd_path)) {
      exit(1);
    }
    if (bbc_econet != 0) {
      int bound = BbcMachineOpenEconet(interpreter.bbc, bbc_econet, bbc_econet_port);
      fprintf(stderr, "Econet station %d\n", bbc_econet);
      if (bound < 0) {
        fprintf(stderr, "Econet socket is not available\n");
      }
    }
    if (disc_socket) {
      int bound = BbcMachineListenDiscs(interpreter.bbc, disc_port);
      if (bound < 0) {
        fprintf(stderr, "Disc socket is not available\n");
      } else {
        fprintf(stderr, "Disc socket: 127.0.0.1:%d\n", bound);
      }
    }
    if (tape_socket) {
      int bound = BbcMachineListenTapes(interpreter.bbc, tape_port);
      if (bound < 0) {
        fprintf(stderr, "Tape socket is not available\n");
      } else {
        fprintf(stderr, "Tape socket: 127.0.0.1:%d\n", bound);
      }
    }
    if (joystick_socket) {
      int bound = BbcMachineListenJoystick(interpreter.bbc, joystick_port);
      if (bound < 0) {
        fprintf(stderr, "Joystick socket is not available\n");
      } else {
        fprintf(stderr, "Joystick socket: 127.0.0.1:%d\n", bound);
      }
    }
    if (fdc_kind != 0) {
      W65C02InterpreterBbcSetFdc(&interpreter, fdc_kind);
    } else if (filing != BBC_FS_ANY) {
      W65C02InterpreterBbcSetFdc(&interpreter, BbcMachineControllerForFiling(interpreter.bbc, filing));
    } else if (BbcMachineIsMaster(interpreter.bbc)) {
      W65C02InterpreterBbcSetFdc(&interpreter, BBC_FDC_1770);
    }
    if (bbc_65c02 || BbcMachineIsMaster(interpreter.bbc)) {
      W65C02InterpreterBbcUse65C02(&interpreter, true);
    }
    if (bbc_tape != NULL && !BbcMachineLoadTape(interpreter.bbc, bbc_tape)) {
      fprintf(stderr, "Unable to load tape %s\n", bbc_tape);
      exit(1);
    }
    if (bbc_printer != NULL) {
      BbcMachineSetPrinter(interpreter.bbc, bbc_printer);
    }
    if (bbc_shift && interpreter.bbc != NULL) {
      BbcMachineSetKey(interpreter.bbc, 0, 0, true);
    }
  }
  interpreter.bbc_step_limit = bbc_step_limit;
  interpreter.bbc_ram_path = bbc_ram;
  if (!W65C02InterpreterPrepareBbc(&interpreter)) {
    fprintf(stderr, "Unable to start the BBC Micro\n");
    exit(1);
  }
  if (debug) {
    interpreter.stop_at_next_instruction = true;
  }
  while (interpreter.running) {
    if (interpreter.bbc_step_limit != 0 &&
        interpreter.bbc_steps >= interpreter.bbc_step_limit) {
      fprintf(stderr, "bbc: stopped after %llu instructions\n",
              (unsigned long long)interpreter.bbc_steps);
      break;
    }
    W65C02InterpreterStep(&interpreter);
    interpreter.bbc_steps++;
  }
  W65C02InterpreterDestruct(&interpreter);
  BbcRomFileFree(rom_files, rom_file_count);
  free(rom_dir);
  return 0;
}

