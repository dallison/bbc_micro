//
//  main.c
//  6502_interpreter
//
//  Created by David Allison on 5/18/19.
//  Copyright © 2019 David Allison. All rights reserved.
//

#include <limits.h>
#include <stdio.h>
#include "loader.h"
#include "loader_arch_6502.h"
#include "6502_interpreter.h"
#include "bbc_hardware.h"
#include "cassette.h"
#include "cumana.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern bool print_libraries_only;

static void Usage() {
  fprintf(stderr,
          "usage: 6502 [-d] [-x outfile] [-rom file]\n"
          "           [-bbc] [-bbc-machine b|master|master128|master256]\n"
          "           [-bbc-sram spec] [-bbc-mode 0-7] [-bbc-screen file.ppm]\n"
          "           [-bbc-os file] [-bbc-rom slot,file] [-bbc-roms dir]\n"
          "           [-bbc-disc file] [-bbc-disc-port n] [-bbc-fdc 8271|1770]\n"
          "           [-bbc-fs dfs|adfs]\n"
          "           [-bbc-65c02] [-bbc-tape file] [-bbc-tape-port n]\n"
          "           [-bbc-printer file] [-bbc-2mhz] [-bbc-mhz n] filename\n"
          "       With -bbc, Model B ROMs are read from bbc_b_rom_sockets and\n"
          "       Master ROMs from bbc_master_rom_sockets. os.rom or\n"
          "       os-<name>.rom is the OS. <socket>-<name>.rom is sideways\n"
          "       socket 0-15. -bbc-os and -bbc-rom replace one of those files.\n"
          "       master uses a 65SC12, a WD1770, and sideways RAM in sockets\n"
          "       4-7. master256 makes every socket RAM. -bbc-sram 4, 8, 16,\n"
          "       or a list such as 4-7 replaces that set.\n"
          "       A Model B with an OS ROM is an NMOS 6502. -bbc-65c02 keeps\n"
          "       the CMOS opcodes and the davecc $EF syscall.\n"
          "       DFS and ADFS ROMs are both fitted. The drive follows the one\n"
          "       the OS calls. -bbc-fs dfs or -bbc-fs adfs loads only that\n"
          "       filing system.\n"
          "       cumana inserts a disc while -bbc is running. It connects to\n"
          "       127.0.0.1:8177. -bbc-disc-port 0 closes that socket.\n"
          "       cassette inserts a tape. It connects to 127.0.0.1:8178.\n"
          "       -bbc-tape-port 0 closes that socket. -bbc-tape still loads a\n"
          "       tape before the machine starts.\n"
          "       A busy program runs ahead of the wall clock. -bbc-2mhz holds\n"
          "       the CPU at 2 MHz. -bbc-mhz n holds it at n MHz, from 1 to\n"
          "       16, with the timers and the video still in step.\n");
  exit(1);
}

static bool PathIsFile(const char* path) {
  struct stat st;
  return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void SetPathDirectory(String* out, const char* path) {
  const char* slash = strrchr(path, '/');
  if (slash == NULL) {
    StringSet(out, ".");
  } else if (slash == path) {
    StringSet(out, "/");
  } else {
    StringInitFromSegment(out, path, (size_t)(slash - path));
  }
}

static bool SetInvocationPath(String* out, const char* argv0) {
  if (strchr(argv0, '/') != NULL) {
    if (argv0[0] == '/') {
      StringSet(out, argv0);
      return true;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      return false;
    }
    StringPrintf(out, "%s/%s", cwd, argv0);
    return true;
  }

  const char* path_env = getenv("PATH");
  if (path_env == NULL) {
    return false;
  }
  char* paths = strdup(path_env);
  if (paths == NULL) {
    return false;
  }
  bool found = false;
  char* save = NULL;
  for (char* dir = strtok_r(paths, ":", &save); dir != NULL;
       dir = strtok_r(NULL, ":", &save)) {
    String candidate = {0};
    StringPrintf(&candidate, "%s/%s", dir, argv0);
    if (access(candidate.value, X_OK) == 0) {
      StringSetString(out, &candidate);
      found = true;
      StringDestruct(&candidate);
      break;
    }
    StringDestruct(&candidate);
  }
  free(paths);
  return found;
}

static bool TryROMPath(String* out, const char* base, const char* relative) {
  if (base == NULL || base[0] == '\0') {
    return false;
  }
  String candidate = {0};
  if (relative == NULL || relative[0] == '\0') {
    StringInit(&candidate, base);
  } else {
    StringPrintf(&candidate, "%s/%s", base, relative);
  }
  bool found = false;
  if (PathIsFile(candidate.value)) {
    char resolved[PATH_MAX];
    if (realpath(candidate.value, resolved) != NULL) {
      StringSet(out, resolved);
      found = true;
    }
  }
  StringDestruct(&candidate);
  return found;
}

static bool FindDefaultROM(String* rom, const char* argv0) {
  if (TryROMPath(rom, getenv("DAVECC_6502_ROM"), NULL)) {
    return true;
  }

  const char* root = getenv("DAVECC_ROOT");
  const char* root_relatives[] = {
      "bazel-bin/6502_support/6502rom.exe",
      "lib/davecc/6502rom.exe",
      "share/davecc/6502rom.exe",
  };
  for (size_t i = 0; i < sizeof(root_relatives) / sizeof(root_relatives[0]);
       i++) {
    if (TryROMPath(rom, root, root_relatives[i])) {
      return true;
    }
  }

  String invocation = {0};
  String invocation_dir = {0};
  String executable_dir = {0};
  if (SetInvocationPath(&invocation, argv0)) {
    SetPathDirectory(&invocation_dir, invocation.value);
    char resolved[PATH_MAX];
    if (realpath(invocation.value, resolved) != NULL) {
      SetPathDirectory(&executable_dir, resolved);
    }
  }
  StringDestruct(&invocation);

  String* roots[] = {&invocation_dir, &executable_dir};
  const char* relatives[] = {
      "6502_support/6502rom.exe",
      "6502rom.exe",
      "../lib/davecc/6502rom.exe",
      "../share/davecc/6502rom.exe",
  };
  bool found = false;
  for (size_t i = 0; !found && i < sizeof(roots) / sizeof(roots[0]); i++) {
    for (size_t j = 0; j < sizeof(relatives) / sizeof(relatives[0]); j++) {
      if (TryROMPath(rom, roots[i]->value, relatives[j])) {
        found = true;
        break;
      }
    }
  }
  StringDestruct(&invocation_dir);
  StringDestruct(&executable_dir);
  if (found) {
    return true;
  }
  return TryROMPath(rom, ".", "bazel-bin/6502_support/6502rom.exe");
}

int main(int argc, char *argv[]) {
  const char* file = NULL;
  int program_arg_offset = 1;
  bool disassemble_only = false;
  bool extract = false;
  const char* extract_filename = NULL;
  bool debug = false;
  bool cycle_accurate = false;
  bool trace = false;
  bool bbc = false;
  int bbc_mode = -1;
  const char* bbc_screen = NULL;
  const char* bbc_os = NULL;
  const char* bbc_roms = NULL;
  int rom_slots[16];
  const char* rom_paths[16];
  int rom_count = 0;
  const char* disc_paths[2] = {NULL, NULL};
  int disc_next = 0;
  int disc_port = CUMANA_PORT;
  bool disc_socket = true;
  int tape_port = CASSETTE_PORT;
  bool tape_socket = true;
  int fdc_kind = 0;
  int filing = BBC_FS_ANY;
  bool bbc_65c02 = false;
  int bbc_mhz = 0;
  int bbc_model = BBC_MACHINE_B;
  bool bbc_sram_set = false;
  bool bbc_sram[16];
  const char* bbc_tape = NULL;
  const char* bbc_printer = NULL;
  const char* rom_filename = NULL;
  for (int i = 1; i < argc; i++) {
    // Once the executable is known, every remaining token belongs to the
    // guest, including arguments such as "-d" that resemble interpreter
    // options.
    if (file != NULL) {
      continue;
    }
    if (argv[i][0] == '-') {
      if (strcmp(argv[i], "-debug") == 0) {
        debug = true;
      } else if (strcmp(argv[i], "-trace") == 0) {
          trace = true;
      } else if (strcmp(argv[i], "-cycle") == 0) {
        cycle_accurate = true;
      } else if (argv[i][1] == 'd') {
        disassemble_only = true;
      } else if (argv[i][1] == 'x') {
          extract = true;
        if (i == argc - 1) {
          Usage();
        }
        extract_filename = argv[++i];
      } else if (strcmp(argv[i], "-rom") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        rom_filename = argv[++i];
      } else if (strcmp(argv[i], "-bbc") == 0) {
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-mode") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        const char* mode_text = argv[++i];
        if (mode_text[0] < '0' || mode_text[0] > '7' || mode_text[1] != '\0') {
          Usage();
        }
        bbc_mode = mode_text[0] - '0';
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-screen") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_screen = argv[++i];
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-os") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_os = argv[++i];
        bbc = true;
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
        bbc = true;
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
        bbc = true;
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
        bbc = true;
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
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-fs") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        filing = BbcMachineParseFilingSystem(argv[++i]);
        if (filing < 0) {
          Usage();
        }
        bbc = true;
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
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-machine") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_model = BbcMachineParseModel(argv[++i]);
        if (bbc_model < 0) {
          Usage();
        }
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-sram") == 0) {
        if (i == argc - 1 || !BbcMachineParseSidewaysRam(argv[++i], bbc_sram)) {
          Usage();
        }
        bbc_sram_set = true;
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-roms") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_roms = argv[++i];
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-65c02") == 0) {
        bbc_65c02 = true;
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-2mhz") == 0) {
        bbc_mhz = 2;
        cycle_accurate = true;
        bbc = true;
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
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-tape") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_tape = argv[++i];
        bbc = true;
      } else if (strcmp(argv[i], "-bbc-printer") == 0) {
        if (i == argc - 1) {
          Usage();
        }
        bbc_printer = argv[++i];
        bbc = true;
      } else {
        Usage();
      }
    } else {
      if (file == NULL) {
        file = argv[i];
        program_arg_offset = i+1;
      }
    }
  }
  if (file == NULL) {
    fprintf(stderr, "usage: need a file to execute\n");
    exit(1);
  }
  String filename;
  StringInit(&filename, file);
  
  W65C02Interpreter interpreter;
  Loader loader;
  String default_rom = {0};
  
  char* rom_dir = NULL;
  BbcRomFile rom_files[BBC_ROM_IMAGE_MAX];
  int rom_file_count = 0;
  bool skip_slot[16] = {false};
  if (bbc) {
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
  if (rom_filename == NULL && bbc_os == NULL) {
    if (!FindDefaultROM(&default_rom, argv[0])) {
      fprintf(stderr,
              "Unable to find the 65C02 support ROM; pass -rom <file>, set "
              "DAVECC_6502_ROM or DAVECC_ROOT, or build "
              "//:support_rom_65c02\n");
      exit(1);
    }
    rom_filename = default_rom.value;
  }
  W65C02InterpreterInit(&interpreter, debug, cycle_accurate, trace, rom_filename);
  if (bbc_mhz > 0 && !W65C02InterpreterSetClockMhz(&interpreter, bbc_mhz)) {
    Usage();
  }
  StringDestruct(&default_rom);
  if (bbc) {
    int r;
    W65C02InterpreterUseBbc(&interpreter, bbc_mode, bbc_screen, bbc_os);
    if (interpreter.bbc == NULL || !BbcMachineSetModel(interpreter.bbc, bbc_model)) {
      fprintf(stderr, "Unable to select the BBC machine\n");
      exit(1);
    }
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
  }
  
  // The LD_TRACE_LOADED_OBJECTS variable shows the loaded objects
  // and doesn't run the program.
  char* ld_trace = getenv("LD_TRACE_LOADED_OBJECTS");
  if (ld_trace != NULL && ld_trace[0] != '\0') {
    print_libraries_only = true;
  }
  
  // Initialize a 6502 architecture.
  LoaderArchitecture arch;
  W65C02LoaderArchitectureInit(&arch);
  
  // Initialize the loader from the given exe file.
  bool ok = LoaderInitFromFile(&loader, &filename, 0,
                               &arch,
                               NULL,
                               ".");
  if (!ok) {
    printf("Error Loading %s\n", filename.value);
    exit(1);
  }
  
  if (print_libraries_only) {
    exit(0);
  }
  
 
  int result = 0;
  if (disassemble_only) {
    W65C02InterpreterDisassemble(&interpreter, &loader);
  } else {
    // The 6502 interpreter runs the guest inside a 64K memory array at the
    // ELF linked addresses, so the entry point must be the guest-linked entry
    // (e_entry).  The shared loader rewrites loader.main_address to a host
    // runtime pointer for ignore_vaddr architectures, which is meaningless for
    // the in-array 6502, so use the original ELF entry directly.
    uint64_t entry = loader.elf_file != NULL ? loader.elf_file->header->entry
                                             : loader.main_address;
    result = W65C02InterpreterRun(&interpreter, &loader, entry, argc, argv,
                                  program_arg_offset);
  }
  
  if (extract) {
    FILE* fp = fopen(extract_filename, "w");
    W65C02InterpreterExtract(&interpreter, &loader, fp);
    fclose(fp);
    printf("Extracted to %s\n", extract_filename);
  }
  W65C02InterpreterDestruct(&interpreter);
  LoaderDestruct(&loader);
  BbcRomFileFree(rom_files, rom_file_count);
  free(rom_dir);
  return result;
}

