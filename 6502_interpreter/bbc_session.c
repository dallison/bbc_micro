#include "bbc_session.h"

#include "bbc_hardware.h"
#include "cassette.h"
#include "cumana.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

W65C02Interpreter g_cpu;
bool g_ready = false;

static char* g_rom_dir = NULL;
static double g_screen_scale = 1.0;
static bool g_game_caps = false;
static int g_keyboard = BBC_KEYBOARD_UK;
static bool g_keyboard_explicit = false;
static BbcRomFile g_rom_files[BBC_ROM_IMAGE_MAX];
static int g_rom_file_count = 0;

static void Usage(void) {
  fprintf(stderr,
          "usage: bbc [-machine b|master|master128|master256] [-sram spec]\n"
          "           [-os file] [-rom slot,file] [-roms dir] [-mode 0-7]\n"
          "           [-disc file] [-disc0 file] [-disc1 file] [-disc-port n]\n"
          "           [-hd file]\n"
          "           [-fdc 8271|1770] [-fs dfs|adfs] [-65c02]\n"
          "           [-tape file] [-tape-port n]\n"
          "           [-printer file] [-mhz n] [-turbo] [-volume n] [-game-caps]\n"
          "           [-keyboard uk|us]\n"
          "           [-econet station] [-econet-port n]\n"
          "       The default machine is the Model B. Its ROMs come from\n"
          "       bbc_b_rom_sockets. master and master128 use\n"
          "       bbc_master_rom_sockets, a 65SC12, a WD1770, and sideways RAM\n"
          "       in sockets 4-7. master256 makes every sideways socket RAM.\n"
          "       -sram 4, 8, 16, or a list such as 4-7 replaces that set.\n"
          "       os.rom or os-<name>.rom is the OS. <socket>-<name>.rom is\n"
          "       sideways socket 0-15. -os and -rom replace one of those files.\n"
          "       The Model B CPU is the NMOS 6502. -65c02 keeps the CMOS opcodes\n"
          "       and the davecc $EF syscall. F1 to F9 are the BBC keys f1 to f9,\n"
          "       and F10 is f0. F12 resets the machine.\n"
#ifdef __APPLE__
          "       While this window is in front, that row is the BBC keys.\n"
#endif
          "       DFS and ADFS ROMs are both fitted. The drive follows the one\n"
          "       the OS calls. -fs dfs or -fs adfs loads only that filing\n"
          "       system.\n"
          "       cumana inserts a disc while the machine is running. It\n"
          "       connects to 127.0.0.1:8177. -disc-port chooses another port,\n"
          "       and -disc-port 0 closes the socket. -disc still mounts an\n"
          "       image before startup.\n"
          "       -hd file mounts an ADFS hard disc, a file of 256-byte\n"
          "       sectors. It is drive 0, and ADFS is the filing system that\n"
          "       starts. *CAT or *MOUNT 0 selects it. Drives 4 and 5 stay\n"
          "       the floppies.\n"
          "       cassette inserts a tape and records guest saves back into\n"
          "       the file. It connects to 127.0.0.1:8178. -tape-port 0 closes\n"
          "       that socket. -tape still loads a tape before startup.\n"
          "       The CPU runs at 2 MHz, in step with the timers and the video.\n"
          "       -mhz n selects another fixed rate, from 1 to 16. -2mhz is\n"
          "       the same as -mhz 2. -turbo lets a busy program run ahead\n"
          "       of the wall clock. -volume n is 0 for silence through 11\n"
          "       for full volume. The default is 7.\n"
          "       Each Caps Lock press toggles the BBC lock. -game-caps keeps\n"
          "       the key down only while it is held, which games such as\n"
          "       Zalaga read directly.\n"
          "       -keyboard uk draws the BBC MODE 7 punctuation. { and } are\n"
          "       the fraction signs there. -keyboard us draws the braces and\n"
          "       brackets labelled on a US keyboard. With no flag, the choice\n"
          "       follows the host keyboard.\n"
#if defined(__APPLE__)
          "       -scale n sets the window size. 1 is 800 by 640. n is greater\n"
          "       than 0 and at most 4, so 1.5 is one and a half times that\n"
          "       size. The picture keeps its shape and the pixels grow with\n"
          "       the window.\n"
#elif defined(__linux__)
          "       -scale n sets the window size. 1 is the picture's own size.\n"
          "       n is greater than 0 and at most 4, so 1.5 is one and a half\n"
          "       times that size. The picture keeps its shape and the pixels\n"
          "       grow with the window.\n"
#endif
          "       -econet n fits the Econet interface as station n (1-254).\n"
          "       That is the 68B54 at &FEA0, not the RS423 serial port.\n"
          "       Stations that use the same -econet-port share a wire. The\n"
          "       default port is 8179. The network clock is present.\n");
}

static void ReleaseRoms(void) {
  BbcRomFileFree(g_rom_files, g_rom_file_count);
  g_rom_file_count = 0;
  free(g_rom_dir);
  g_rom_dir = NULL;
}

double BbcSessionScreenScale(void) { return g_screen_scale; }

bool BbcSessionGameCaps(void) { return g_game_caps; }

void BbcSessionPreferKeyboard(int kind) {
  if (g_keyboard_explicit) {
    return;
  }
  g_keyboard = kind == BBC_KEYBOARD_US ? BBC_KEYBOARD_US : BBC_KEYBOARD_UK;
  if (g_cpu.bbc != NULL) {
    BbcMachineSetKeyboard(g_cpu.bbc, g_keyboard);
  }
}

void BbcSessionFinish(void) {
  g_ready = false;
  g_cpu.running = false;
  W65C02InterpreterDestruct(&g_cpu);
  ReleaseRoms();
}

bool BbcSessionStart(int argc, char** argv, int* status) {
  const char* os_path = NULL;
  int mode = -1;
  bool selftest = false;
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
  int fdc_kind = 0;
  int filing = BBC_FS_ANY;
  bool use_65c02 = false;
  int clock_mhz = 2;
  bool run_ahead = false;
  const char* tape_path = NULL;
  const char* printer_path = NULL;
  int volume = 7;
  int econet_station = 0;
  int econet_port = BBC_ECONET_PORT;
  const char* roms_arg = NULL;
  int machine = BBC_MACHINE_B;
  bool sram_set = false;
  bool sram[16];
  bool skip_slot[16];
  bool cpu_inited = false;
  int i;
  memset(skip_slot, 0, sizeof(skip_slot));
  g_ready = false;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-os") == 0 || strcmp(argv[i], "-bbc-os") == 0) {
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      os_path = argv[++i];
    } else if (strcmp(argv[i], "-mode") == 0 || strcmp(argv[i], "-bbc-mode") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] < '0' || argv[i + 1][0] > '7' || argv[i + 1][1] != '\0') {
        Usage();
        *status = 1;
        return false;
      }
      mode = argv[++i][0] - '0';
    } else if (strcmp(argv[i], "-rom") == 0 || strcmp(argv[i], "-bbc-rom") == 0) {
      const char* spec;
      const char* comma;
      char slot_text[8];
      size_t slot_len;
      int slot;
      if (i + 1 >= argc || rom_count == 16) {
        Usage();
        *status = 1;
        return false;
      }
      spec = argv[++i];
      comma = strchr(spec, ',');
      if (comma == NULL || comma == spec || comma[1] == '\0') {
        Usage();
        *status = 1;
        return false;
      }
      slot_len = (size_t)(comma - spec);
      if (slot_len >= sizeof(slot_text)) {
        Usage();
        *status = 1;
        return false;
      }
      memcpy(slot_text, spec, slot_len);
      slot_text[slot_len] = '\0';
      slot = atoi(slot_text);
      if (slot < 0 || slot > 15) {
        Usage();
        *status = 1;
        return false;
      }
      rom_slots[rom_count] = slot;
      rom_paths[rom_count] = comma + 1;
      rom_count++;
    } else if (strcmp(argv[i], "-disc") == 0 || strcmp(argv[i], "-disc0") == 0 ||
               strcmp(argv[i], "-disc1") == 0) {
      int drive;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      if (strcmp(argv[i], "-disc1") == 0) {
        drive = 1;
      } else if (strcmp(argv[i], "-disc0") == 0) {
        drive = 0;
      } else {
        drive = disc_next < 2 ? disc_next : 1;
      }
      disc_paths[drive] = argv[++i];
      if (drive >= disc_next && disc_next < 2) {
        disc_next = drive + 1;
      }
    } else if (strcmp(argv[i], "-hd") == 0 || strcmp(argv[i], "-bbc-hd") == 0) {
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      hd_path = argv[++i];
    } else if (strcmp(argv[i], "-disc-port") == 0) {
      char* end = NULL;
      long value;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      value = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || value < 0 || value > 65535) {
        Usage();
        *status = 1;
        return false;
      }
      if (value == 0) {
        disc_socket = false;
      } else {
        disc_port = (int)value;
      }
    } else if (strcmp(argv[i], "-tape-port") == 0 || strcmp(argv[i], "-bbc-tape-port") == 0) {
      char* end = NULL;
      long value;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      value = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || value < 0 || value > 65535) {
        Usage();
        *status = 1;
        return false;
      }
      if (value == 0) {
        tape_socket = false;
      } else {
        tape_port = (int)value;
      }
    } else if (strcmp(argv[i], "-fs") == 0 || strcmp(argv[i], "-bbc-fs") == 0) {
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      filing = BbcMachineParseFilingSystem(argv[++i]);
      if (filing < 0) {
        Usage();
        *status = 1;
        return false;
      }
    } else if (strcmp(argv[i], "-fdc") == 0) {
      const char* which;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      which = argv[++i];
      if (strcmp(which, "8271") == 0) {
        fdc_kind = BBC_FDC_8271;
      } else if (strcmp(which, "1770") == 0) {
        fdc_kind = BBC_FDC_1770;
      } else {
        Usage();
        *status = 1;
        return false;
      }
    } else if (strcmp(argv[i], "-machine") == 0 || strcmp(argv[i], "-bbc-machine") == 0) {
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      machine = BbcMachineParseModel(argv[++i]);
      if (machine < 0) {
        Usage();
        *status = 1;
        return false;
      }
    } else if (strcmp(argv[i], "-sram") == 0 || strcmp(argv[i], "-bbc-sram") == 0) {
      if (i + 1 >= argc || !BbcMachineParseSidewaysRam(argv[++i], sram)) {
        Usage();
        *status = 1;
        return false;
      }
      sram_set = true;
    } else if (strcmp(argv[i], "-roms") == 0 || strcmp(argv[i], "-bbc-roms") == 0) {
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      roms_arg = argv[++i];
    } else if (strcmp(argv[i], "-65c02") == 0 || strcmp(argv[i], "-bbc-65c02") == 0) {
      use_65c02 = true;
    } else if (strcmp(argv[i], "-tape") == 0 || strcmp(argv[i], "-bbc-tape") == 0) {
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      tape_path = argv[++i];
    } else if (strcmp(argv[i], "-printer") == 0 || strcmp(argv[i], "-bbc-printer") == 0) {
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      printer_path = argv[++i];
    } else if (strcmp(argv[i], "-2mhz") == 0) {
      clock_mhz = 2;
      run_ahead = false;
    } else if (strcmp(argv[i], "-volume") == 0) {
      char* end = NULL;
      long value;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      value = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || value < 0 || value > 11) {
        Usage();
        *status = 1;
        return false;
      }
      volume = (int)value;
    } else if (strcmp(argv[i], "-scale") == 0) {
      char* end = NULL;
      double value;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      value = strtod(argv[++i], &end);
      if (end == argv[i] || *end != '\0' || !(value > 0.0 && value <= 4.0)) {
        Usage();
        *status = 1;
        return false;
      }
      g_screen_scale = value;
    } else if (strcmp(argv[i], "-econet") == 0) {
      char* end = NULL;
      long value;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      value = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || value < 1 || value > 254) {
        Usage();
        *status = 1;
        return false;
      }
      econet_station = (int)value;
    } else if (strcmp(argv[i], "-econet-port") == 0) {
      char* end = NULL;
      long value;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      value = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || value < 1 || value > 65535) {
        Usage();
        *status = 1;
        return false;
      }
      econet_port = (int)value;
    } else if (strcmp(argv[i], "-game-caps") == 0 || strcmp(argv[i], "-bbc-game-caps") == 0) {
      g_game_caps = true;
    } else if (strcmp(argv[i], "-keyboard") == 0 || strcmp(argv[i], "-bbc-keyboard") == 0) {
      int kind;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      kind = BbcMachineParseKeyboard(argv[++i]);
      if (kind < 0) {
        Usage();
        *status = 1;
        return false;
      }
      g_keyboard = kind;
      g_keyboard_explicit = true;
    } else if (strcmp(argv[i], "-turbo") == 0) {
      run_ahead = true;
    } else if (strcmp(argv[i], "-mhz") == 0) {
      char* end = NULL;
      long value;
      if (i + 1 >= argc) {
        Usage();
        *status = 1;
        return false;
      }
      value = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || !W65C02InterpreterSetClockMhz(&g_cpu, (int)value)) {
        Usage();
        *status = 1;
        return false;
      }
      clock_mhz = (int)value;
      run_ahead = false;
    } else if (strcmp(argv[i], "-selftest") == 0) {
      selftest = true;
    } else if (strcmp(argv[i], "-h") == 0) {
      Usage();
      *status = 0;
      return false;
    } else {
      Usage();
      *status = 1;
      return false;
    }
  }

  if (roms_arg != NULL) {
    g_rom_file_count = BbcMachineListRomDirectory(roms_arg, g_rom_files, BBC_ROM_IMAGE_MAX, filing);
    if (g_rom_file_count < 0) {
      g_rom_file_count = 0;
      *status = 1;
      return false;
    }
  } else if (!selftest) {
    g_rom_dir = BbcMachineFindRomDirectory(argv[0], BbcMachineRomDirectoryName(machine));
    if (g_rom_dir != NULL) {
      g_rom_file_count =
          BbcMachineListRomDirectory(g_rom_dir, g_rom_files, BBC_ROM_IMAGE_MAX, filing);
      if (g_rom_file_count < 0) {
        g_rom_file_count = 0;
        ReleaseRoms();
        *status = 1;
        return false;
      }
    }
  }
  if (os_path == NULL) {
    os_path = BbcRomOsPath(g_rom_files, g_rom_file_count);
    if (os_path != NULL) {
      fprintf(stderr, "ROM os: %s\n", os_path);
    }
  }
  W65C02InterpreterInit(&g_cpu, false, true, false, NULL);
  cpu_inited = true;
  if (!run_ahead && !W65C02InterpreterSetClockMhz(&g_cpu, clock_mhz)) {
    Usage();
    goto fail;
  }
  W65C02InterpreterUseBbc(&g_cpu, mode, NULL, os_path);
  if (g_cpu.bbc == NULL || !BbcMachineSetModel(g_cpu.bbc, machine)) {
    fprintf(stderr, "Unable to select the BBC machine\n");
    goto fail;
  }
  BbcMachineSetKeyboard(g_cpu.bbc, g_keyboard);
  if (sram_set && !BbcMachineSetSidewaysRam(g_cpu.bbc, sram)) {
    fprintf(stderr, "Unable to allocate sideways RAM\n");
    goto fail;
  }
  for (i = 0; i < rom_count; i++) {
    if (!W65C02InterpreterBbcLoadRom(&g_cpu, rom_slots[i], rom_paths[i])) {
      goto fail;
    }
    skip_slot[rom_slots[i]] = true;
  }
  if (!BbcMachineLoadRomFiles(g_cpu.bbc, g_rom_files, g_rom_file_count, skip_slot)) {
    goto fail;
  }
  for (i = 0; i < 2; i++) {
    if (disc_paths[i] != NULL && !W65C02InterpreterBbcLoadDisc(&g_cpu, i, disc_paths[i])) {
      goto fail;
    }
  }
  if (hd_path != NULL && !W65C02InterpreterBbcLoadHardDisc(&g_cpu, hd_path)) {
    goto fail;
  }
  if (disc_socket) {
    int bound = BbcMachineListenDiscs(g_cpu.bbc, disc_port);
    if (bound < 0) {
      fprintf(stderr, "Disc socket is not available\n");
    } else {
      fprintf(stderr, "Disc socket: 127.0.0.1:%d\n", bound);
    }
  }
  if (tape_socket) {
    int bound = BbcMachineListenTapes(g_cpu.bbc, tape_port);
    if (bound < 0) {
      fprintf(stderr, "Tape socket is not available\n");
    } else {
      fprintf(stderr, "Tape socket: 127.0.0.1:%d\n", bound);
    }
  }
  if (fdc_kind != 0) {
    W65C02InterpreterBbcSetFdc(&g_cpu, fdc_kind);
  } else if (filing != BBC_FS_ANY) {
    W65C02InterpreterBbcSetFdc(&g_cpu, BbcMachineControllerForFiling(g_cpu.bbc, filing));
  } else if (BbcMachineIsMaster(g_cpu.bbc)) {
    W65C02InterpreterBbcSetFdc(&g_cpu, BBC_FDC_1770);
  }
  if (use_65c02 || BbcMachineIsMaster(g_cpu.bbc)) {
    W65C02InterpreterBbcUse65C02(&g_cpu, true);
  }
  if (tape_path != NULL && !BbcMachineLoadTape(g_cpu.bbc, tape_path)) {
    fprintf(stderr, "Unable to load tape %s\n", tape_path);
    goto fail;
  }
  if (printer_path != NULL) {
    BbcMachineSetPrinter(g_cpu.bbc, printer_path);
  }
  BbcMachineSetVolume(g_cpu.bbc, volume);
  if (econet_station != 0) {
    int bound = BbcMachineOpenEconet(g_cpu.bbc, econet_station, econet_port);
    fprintf(stderr, "Econet station %d\n", econet_station);
    if (bound < 0) {
      fprintf(stderr, "Econet socket is not available\n");
    }
  }
  if (!W65C02InterpreterPrepareBbc(&g_cpu)) {
    fprintf(stderr, "Unable to start the BBC Micro\n");
    goto fail;
  }
  if (selftest) {
    int cycles = 0;
    int guard = 0;
    int lit = 0;
    int energy = 0;
    int n;
    int s;
    int16_t audio[4096];
    const uint8_t* fb;
    while (cycles < 120000 && guard < 1000000) {
      int step = W65C02InterpreterStep(&g_cpu);
      if (step <= 0) {
        break;
      }
      cycles += step;
      guard++;
    }
    if (BbcMachineCompletedFrames(g_cpu.bbc) == 0) {
      BbcMachineRender(g_cpu.bbc);
    }
    fb = BbcFramebuffer(g_cpu.bbc);
    for (s = 0; s < BBC_FB_WIDTH * BbcFrameHeight(g_cpu.bbc) * 3; s++) {
      if (fb[s] != 0) {
        lit++;
      }
    }
    n = BbcMachineReadAudio(g_cpu.bbc, audio, 4096);
    for (s = 0; s < n; s++) {
      energy += audio[s] < 0 ? -audio[s] : audio[s];
    }
    fprintf(stderr, "selftest cycles=%d lit=%d audio=%d energy=%d\n", cycles, lit, n, energy);
    *status = (lit > 100 && energy > 100000) ? 0 : 1;
    W65C02InterpreterDestruct(&g_cpu);
    ReleaseRoms();
    return false;
  }
  g_ready = true;
  *status = 0;
  return true;

fail:
  if (cpu_inited) {
    W65C02InterpreterDestruct(&g_cpu);
  }
  ReleaseRoms();
  *status = 1;
  return false;
}
