//
//  bbc_hardware.c
//  BBC Micro Model B: SHEILA, FRED, JIM, the video ULA, and a framebuffer.
//

#include "bbc_hardware.h"
#include "bbc_font.h"
#include "bbc_platform.h"
#include "cassette.h"
#include "cumana.h"
#include "tube.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BBC_CPU_CYCLES_PER_FRAME 40000
#define BBC_AUDIO_RATE 48000
#define BBC_CPU_HZ 2000000
#define BBC_AUDIO_RING 8192

static const int kSoundAmp[16] = {
    32767, 26028, 20675, 16422, 13045, 10362, 8231, 6538,
    5193,  4125,  3277,  2603,  2067,  1642,  1304, 0};

static const uint8_t kScreenSubtract[4] = {8, 4, 10, 5};

static const uint8_t kRgb[8][3] = {
    {0, 0, 0},       {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
    {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
};

// Values written to the video ULA palette by the MOS for each mode group.
static const uint8_t kPal2[16] = {0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0,
                                  0x07, 0x17, 0x27, 0x37, 0x47, 0x57, 0x67, 0x77};
static const uint8_t kPal4[16] = {0xA0, 0xB0, 0xE0, 0xF0, 0x84, 0x94, 0xC4, 0xD4,
                                  0x26, 0x36, 0x66, 0x76, 0x07, 0x17, 0x47, 0x57};
static const uint8_t kPal16[16] = {0xF8, 0xE9, 0xDA, 0xCB, 0xBC, 0xAD, 0x9E, 0x8F,
                                   0x70, 0x61, 0x52, 0x43, 0x34, 0x25, 0x16, 0x07};

typedef struct {
  uint8_t orb;
  uint8_t ora;
  uint8_t ddrb;
  uint8_t ddra;
  uint8_t t1l_l;
  uint8_t t1l_h;
  uint8_t t2l_l;
  uint8_t sr;
  uint8_t acr;
  uint8_t pcr;
  uint8_t ifr;
  uint8_t ier;
  int t1_counter;
  int t2_counter;
  bool t1_running;
  bool t2_running;
  int sr_bits;
  bool sr_active;
  int sr_period;
} BbcVia;

typedef struct BbcDiscImage {
  uint8_t* data;
  size_t length;
  int tracks;
  int sides;
  int spt;
  bool protect;
  char* path;
  int fd;
} BbcDiscImage;

// A connected cumana waits here until the CPU thread mounts the image.
struct CumanaPending {
  int fd;
  CumanaInsert insert;
  struct CumanaPending* next;
};

struct BbcMachine {
  uint8_t* ram;
  uint8_t fred[256];
  uint8_t* jim;
  uint8_t jim_page;
  uint8_t crtc_addr;
  uint8_t crtc[32];
  uint8_t ula_control;
  uint8_t ula_pal[16];
  uint8_t romsel;
  uint8_t serial_ula;
  uint8_t acia_control;
  uint8_t acia_rx;
  uint8_t acia_tx;
  bool acia_rx_full;
  bool acia_tx_empty;
  bool acia_dcd;
  bool acia_overrun;
  int acia_tx_wait;
  int acia_rx_wait;
  int acia_baud_tx;
  int acia_baud_rx;
  struct {
    uint8_t data;
    uint8_t dcd;
  }* tape;
  size_t tape_len;
  size_t tape_pos;
  uint8_t* printed;
  size_t printed_len;
  size_t printed_cap;
  char* printer_path;
  int printer_ack;
  bool fire[2];
  uint16_t analogue[4];
  int adc_delay;
  bool adc_busy;
  uint8_t adc_hi;
  uint8_t adc_lo;
  bool sys_cb1;
  uint8_t ic32;
  bool ca2_high;
  uint8_t sound_last;
  int sound_writes;
  uint8_t key_down[16][8];
  uint16_t tone_period[3];
  uint8_t tone_vol[4];
  uint8_t sound_latch;
  uint8_t noise_ctrl;
  int sound_phase[4];
  int sound_level[4];
  uint16_t noise_lfsr;
  int64_t audio_acc;
  int16_t audio_ring[BBC_AUDIO_RING];
  int audio_r;
  int audio_w;
  int volume;
  pthread_mutex_t audio_mu;
  bool audio_ready;
  uint8_t adc_status;
  uint16_t adc[4];
  uint8_t tube_status;
  struct Tube* tube;
  void* parasite;
  void (*parasite_run)(void* parasite, int host_cycles);
  void (*parasite_reset)(void* parasite);
  void (*parasite_destroy)(void* parasite);
  uint8_t (*parasite_read)(void* parasite, uint16_t addr);
  uint16_t (*parasite_pc)(void* parasite);
  struct {
    int kind;
    bool forced;
    uint8_t control;
    uint8_t drvout;
    uint8_t status;
    uint8_t command;
    uint8_t data;
    uint8_t result;
    uint8_t params[5];
    int param_i;
    int param_n;
    int drive;
    int side;
    int track[2];
    int sector;
    int sectors_left;
    int sector_size;
    int bytes_left;
    int sector_off;
    int phase;
    int delay;
    int byte_gap;
    int direction;
    int buf_i;
    int spin;
    uint8_t track_reg;
    uint8_t sector_reg;
    bool intrq;
    bool nmi_line;
    bool writing;
    uint8_t buffer[1024];
    uint8_t track_image[8192];
    int track_left;
    // disc[0] and disc[1] are DFS/ADFS drives 0 and 1. extra[0] and extra[1]
    // are DFS drives 2 and 3: the second side of those two drives, used when
    // that side was inserted as its own image.
    BbcDiscImage disc[2];
    BbcDiscImage extra[2];
  } fdc;
  // Acorn Winchester host adapter at &FC40. ADFS drive 0. The image is a
  // raw file of 256-byte sectors. phase 0 is bus free.
  struct {
    uint8_t* data;
    size_t length;
    uint32_t sectors;
    bool protect;
    bool dirty;
    char* path;
    uint8_t latch;
    // &FC43 enables the adapter interrupt. ADFS 1.30 sets it after a sector
    // write and waits until the service interrupt clears the busy flag.
    bool irq;
    int phase;
    int cdb_i;
    int cdb_n;
    uint8_t cdb[16];
    bool image_io;
    size_t pos;
    size_t remaining;
    uint8_t scratch[4];
    int scratch_i;
    uint8_t status_byte;
    uint8_t sense[4];
  } hd;
  BbcVia sys;
  BbcVia user;
  int cycle_acc;
  int via_phase;
  bool fast_clock;
  uint8_t* beam_fb;
  int video_odd;
  int h_count;
  int v_count;
  int scanline;
  uint16_t vaddr;
  uint16_t line_start;
  uint16_t next_line_start;
  bool h_disp;
  bool v_disp;
  bool scan_disp;
  bool in_vsync;
  bool had_vsync_row;
  bool end_of_main;
  bool in_vert_adjust;
  bool check_vert_adjust;
  bool end_of_vert_adjust;
  bool end_of_frame_latched;
  bool beam_started;
  bool beam_size_hold;
  bool line_drawn;
  bool vsync_pin;
  bool first_scanline;
  bool do_even_frame;
  int video_frames;
  int vpulse;
  int vert_adjust;
  int beam_y;
  int beam_width;
  int beam_frames;
  int disp_x;
  int tt_fg;
  int tt_bg;
  bool tt_graphics;
  bool tt_separated;
  bool tt_conceal;
  bool tt_flash;
  bool tt_double;
  bool tt_hold;
  bool tt_held_valid;
  uint8_t tt_held;
  bool tt_bottom;
  bool tt_row_double;
  uint8_t* sideways[16];
  // Virtual images 1-7. Image 0 is sideways[] above. Bits 4-6 of ROMSEL
  // select the image. These are ROM: writes are ignored and are not copied
  // back.
  uint8_t* sideways_page[16][7];
  bool sideways_loaded[16];
  bool sideways_ram[16];
  uint8_t rom_fs[16];
  uint8_t rom_fdc[16];
  bool any_sideways;
  int model;
  int keyboard;
  bool master;
  int mapped_slot;
  int mapped_virt;
  bool andy_mapped;
  bool lynne_mapped;
  bool hazel_mapped;
  bool mos_saved;
  uint16_t last_pc;
  uint8_t acccon;
  uint8_t cmos[64];
  uint8_t cmos_addr;
  bool cmos_ena;
  bool cmos_as;
  bool cmos_d;
  uint8_t* lynne;
  uint8_t* hazel;
  uint8_t* andy;
  uint8_t* mos_low;
  uint8_t* os_rom;
  uint8_t* fb;
  int frame_width;
  int frame_height;
  unsigned frames;
  int disc_listen_fd;
  int disc_listen_port;
  int disc_handshake_fd;
  bool disc_listen_started;
  bool disc_listen_stop;
  pthread_t disc_listen_thread;
  pthread_mutex_t disc_mu;
  struct CumanaPending* disc_pending;
  int disc_attention;
  int disc_watch_acc;
  int tape_fd;
  char* tape_path;
  bool tape_dirty;
  bool tape_protect;
  int tape_listen_fd;
  int tape_listen_port;
  int tape_handshake_fd;
  bool tape_listen_started;
  bool tape_listen_stop;
  pthread_t tape_listen_thread;
  pthread_mutex_t tape_mu;
  struct TapePending* tape_pending;
  int tape_attention;
  int tape_watch_acc;
  // Combined disc and Econet NMI. The 6502 latches one edge.
  bool nmi_line;
  bool nmi_edge;
  struct {
    bool fitted;
    int station;
    int port;
    int fd;
    uint32_t instance;
    bool nmi_enable;
    bool irq;
    uint8_t cr1;
    uint8_t cr2;
    uint8_t cr3;
    uint8_t cr4;
    uint8_t last_sr1;
    uint8_t last_sr2;
    bool fd_latched;
    bool ap;
    bool fv_latched;
    bool err_latched;
    bool ovrn;
    bool rx_abt;
    bool rx_idle;
    bool txu;
    bool fc_latched;
    bool rx_short;
    uint8_t tx_fifo[3];
    uint8_t tx_last[3];
    int tx_count;
    uint8_t tx_frame[2048];
    int tx_len;
    bool tx_in_frame;
    uint8_t rx_fifo[3];
    uint8_t rx_last[3];
    uint8_t rx_first[3];
    int rx_count;
    uint8_t rx_frame[2048];
    int rx_len;
    int rx_pos;
    bool rx_active;
    uint8_t inbox[4][2048];
    int inbox_len[4];
    int inbox_n;
    int bit_acc;
    int poll_acc;
  } econet;
};

struct TapePending {
  int fd;
  CassetteInsert insert;
  struct TapePending* next;
};

static int ScreenSubtract(const BbcMachine* bbc) {
  return kScreenSubtract[(bbc->ic32 >> 4) & 3];
}

uint16_t BbcVideoAddress(const BbcMachine* bbc, uint16_t ma, uint8_t ra) {
  ma &= 0x3fff;
  // MA13 selects the teletext (chunky) map. MODE 7's start address has it set.
  if (ma & 0x2000) {
    uint16_t mem = ma & 0x3ff;
    if (ma & 0x800) {
      mem |= 0x7c00;
    } else {
      mem |= 0x3c00;
    }
    return mem;
  }
  uint16_t low = ma & 0x1fff;
  uint8_t high = (uint8_t)((low >> 8) & 0x0f);
  if (low & 0x1000) {
    high = (uint8_t)((high - ScreenSubtract(bbc)) & 0x0f);
  }
  return (uint16_t)(((high << 11) | ((low & 0xff) << 3) | (ra & 7)) & 0x7fff);
}

static void ViaReset(BbcVia* via) {
  memset(via, 0, sizeof(*via));
}

static void ViaShiftBit(BbcVia* via) {
  int mode = (via->acr >> 2) & 7;
  bool inbound;
  if (!via->sr_active || mode == 0 || mode == 3 || mode == 7) {
    return;
  }
  inbound = mode == 1 || mode == 2;
  if (inbound) {
    via->sr = (uint8_t)((via->sr << 1) | 1);
  } else {
    via->sr = (uint8_t)(via->sr << 1);
  }
  via->sr_bits++;
  if (via->sr_bits < 8) {
    return;
  }
  via->sr_bits = 0;
  via->ifr |= 0x04;
  if (mode != 4) {
    via->sr_active = false;
  }
}

static void ViaAdvance(BbcVia* via, int cycles) {
  int mode;
  int i;
  if (via->t1_running) {
    if (cycles >= via->t1_counter) {
      via->ifr |= 0x40;
      if (via->acr & 0x40) {
        int period = ((via->t1l_h << 8) | via->t1l_l) + 1;
        int extra = cycles - via->t1_counter;
        via->t1_counter = period - (extra % period);
        if (via->t1_counter == 0) {
          via->t1_counter = period;
        }
      } else {
        via->t1_running = false;
        via->t1_counter = 0;
      }
    } else {
      via->t1_counter -= cycles;
    }
  }
  if (via->t2_running) {
    if (cycles >= via->t2_counter) {
      via->ifr |= 0x20;
      mode = (via->acr >> 2) & 7;
      if (via->sr_active && (mode == 1 || mode == 4 || mode == 5)) {
        ViaShiftBit(via);
      }
      if (mode == 4 && via->sr_active && via->sr_period > 0) {
        via->t2_counter = via->sr_period;
        via->t2_running = true;
      } else {
        via->t2_running = false;
        via->t2_counter = 0;
      }
    } else {
      via->t2_counter -= cycles;
    }
  }
  mode = (via->acr >> 2) & 7;
  if (via->sr_active && (mode == 2 || mode == 6)) {
    for (i = 0; i < cycles; i++) {
      ViaShiftBit(via);
    }
  }
}

static void SoundWriteByte(BbcMachine* bbc, uint8_t value) {
  if (value & 0x80) {
    bbc->sound_latch = (uint8_t)((value >> 4) & 7);
    int channel = bbc->sound_latch >> 1;
    if (bbc->sound_latch & 1) {
      bbc->tone_vol[channel] = (uint8_t)(value & 0x0f);
    } else if (channel == 3) {
      bbc->noise_ctrl = (uint8_t)(value & 0x07);
    } else {
      bbc->tone_period[channel] =
          (uint16_t)((bbc->tone_period[channel] & 0x3f0) | (value & 0x0f));
    }
  } else {
    int channel = bbc->sound_latch >> 1;
    if ((bbc->sound_latch & 1) == 0 && channel < 3) {
      bbc->tone_period[channel] = (uint16_t)((bbc->tone_period[channel] & 0x000f) |
                                            ((value & 0x3f) << 4));
    }
  }
  bbc->sound_last = value;
  bbc->sound_writes++;
}

static void SoundPush(BbcMachine* bbc, int16_t sample) {
  if (!bbc->audio_ready) {
    return;
  }
  pthread_mutex_lock(&bbc->audio_mu);
  int next = (bbc->audio_w + 1) % BBC_AUDIO_RING;
  if (next != bbc->audio_r) {
    bbc->audio_ring[bbc->audio_w] = sample;
    bbc->audio_w = next;
  }
  pthread_mutex_unlock(&bbc->audio_mu);
}

static int SoundStep(BbcMachine* bbc, int channel, int period) {
  if (period < 1) {
    period = 1;
  }
  bbc->sound_phase[channel] += 125;
  int limit = 24 * period;
  while (bbc->sound_phase[channel] >= limit) {
    bbc->sound_phase[channel] -= limit;
    bbc->sound_level[channel] ^= 1;
    if (channel == 3) {
      uint16_t shift = bbc->noise_lfsr == 0 ? 0x8000 : bbc->noise_lfsr;
      int feedback = (shift ^ (shift >> 3)) & 1;
      if ((bbc->noise_ctrl & 4) == 0) {
        feedback = 1;
      }
      bbc->noise_lfsr = (uint16_t)((shift >> 1) | (feedback << 15));
    }
  }
  if (channel == 3) {
    return (bbc->noise_lfsr & 1) ? 1 : -1;
  }
  return bbc->sound_level[channel] ? 1 : -1;
}

static void SoundEmit(BbcMachine* bbc) {
  int mix = 0;
  int channel;
  for (channel = 0; channel < 3; channel++) {
    int level = SoundStep(bbc, channel, bbc->tone_period[channel] & 0x3ff);
    mix += level * kSoundAmp[bbc->tone_vol[channel] & 15];
  }
  int noise_period = 0x10 << (bbc->noise_ctrl & 3);
  if ((bbc->noise_ctrl & 3) == 3) {
    noise_period = bbc->tone_period[2] & 0x3ff;
  }
  mix += SoundStep(bbc, 3, noise_period) * kSoundAmp[bbc->tone_vol[3] & 15];
  mix /= 4;
  if (mix > 32767) {
    mix = 32767;
  }
  if (mix < -32768) {
    mix = -32768;
  }
  SoundPush(bbc, (int16_t)mix);
}

static uint8_t ToBcd(int value) {
  if (value < 0) {
    value = 0;
  }
  value %= 100;
  return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static uint8_t RtcRead(BbcMachine* bbc) {
  uint8_t addr = (uint8_t)(bbc->cmos_addr & 0x3f);
  if (addr < 10 && (bbc->cmos[0x0b] & 0x80) == 0) {
    time_t now = time(NULL);
    struct tm tm;
    int value = 0;
    bool binary = (bbc->cmos[0x0b] & 0x04) != 0;
    bool hour24 = (bbc->cmos[0x0b] & 0x02) != 0;
    localtime_r(&now, &tm);
    switch (addr) {
      case 0:
        value = tm.tm_sec;
        break;
      case 2:
        value = tm.tm_min;
        break;
      case 4:
        value = tm.tm_hour;
        if (!hour24) {
          int pm = value >= 12;
          value %= 12;
          if (value == 0) {
            value = 12;
          }
          if (pm) {
            return (uint8_t)((binary ? value : ToBcd(value)) | 0x80);
          }
        }
        break;
      case 6:
        value = tm.tm_wday + 1;
        break;
      case 7:
        value = tm.tm_mday;
        break;
      case 8:
        value = tm.tm_mon + 1;
        break;
      case 9:
        value = tm.tm_year % 100;
        break;
      default:
        return bbc->cmos[addr];
    }
    return binary ? (uint8_t)value : ToBcd(value);
  }
  if (addr == 0x0c) {
    uint8_t flags = bbc->cmos[0x0c];
    bbc->cmos[0x0c] = 0;
    return flags;
  }
  if (addr == 0x0d) {
    return 0x80;
  }
  return bbc->cmos[addr];
}

static void RtcWriteData(BbcMachine* bbc, uint8_t value) {
  uint8_t addr = (uint8_t)(bbc->cmos_addr & 0x3f);
  if (addr == 0x0c || addr == 0x0d) {
    return;
  }
  bbc->cmos[addr] = value;
}

// The HD146818 is wired through the system VIA. PB6 enables the chip, PB7 is
// the address strobe (the address is latched as it falls), IC32 bit 1 is R/W,
// and IC32 bit 2 is the data strobe. A write is taken as that strobe falls.
static void CmosControl(BbcMachine* bbc) {
  bool enabled;
  bool was_as;
  bool was_d;
  bool as;
  bool d;
  bool reading;
  if (!bbc->master) {
    return;
  }
  enabled = (bbc->sys.orb & 0x40) != 0;
  if (!enabled) {
    bbc->cmos_ena = false;
    return;
  }
  bbc->cmos_ena = true;
  was_as = bbc->cmos_as;
  was_d = bbc->cmos_d;
  reading = (bbc->ic32 & 0x02) != 0;
  d = (bbc->ic32 & 0x04) != 0;
  as = (bbc->sys.orb & 0x80) != 0;
  bbc->cmos_as = as;
  bbc->cmos_d = d;
  if (was_as && !as) {
    bbc->cmos_addr = (uint8_t)(bbc->sys.ora & 0x3f);
  }
  if (was_d && !d && !as && !reading) {
    RtcWriteData(bbc, bbc->sys.ora);
  }
}

static uint8_t SystemPortA(BbcMachine* bbc, BbcVia* via) {
  uint8_t pins = (uint8_t)((0xff & (uint8_t)~via->ddra) | (via->ora & via->ddra));
  // The RTC drives the bus only while it is enabled, the address strobe is
  // low, the data strobe is high, and R/W is a read.
  if (bbc->master && bbc->cmos_ena && (via->orb & 0x80) == 0 && (bbc->ic32 & 0x04) != 0 &&
      (bbc->ic32 & 0x02) != 0) {
    pins = (uint8_t)(pins & RtcRead(bbc));
  }
  // IC32 bit 3 low enables the keyboard. The scan code on PA0-PA6 selects
  // the column (low nibble) and row (bits 4-6); PA7 is 1 while that key is down.
  if ((bbc->ic32 & 0x08) == 0) {
    int column = pins & 0x0f;
    int row = (pins >> 4) & 7;
    int down = column < 16 && bbc->key_down[column][row];
    if (!down) {
      pins = (uint8_t)(pins & 0x7f);
    } else if ((via->ddra & 0x80) == 0) {
      pins = (uint8_t)(pins | 0x80);
    }
  }
  return pins;
}

static uint8_t ViaRead(BbcMachine* bbc, BbcVia* via, bool system, int reg) {
  reg &= 0x0f;
  switch (reg) {
    case 0: {
      uint8_t pins = (uint8_t)((0xff & (uint8_t)~via->ddrb) | (via->orb & via->ddrb));
      // PB4 and PB5 are the joystick fire buttons, active low.
      if (system) {
        if ((via->ddrb & 0x10) == 0 && bbc->fire[0]) {
          pins = (uint8_t)(pins & (uint8_t)~0x10);
        }
        if ((via->ddrb & 0x20) == 0 && bbc->fire[1]) {
          pins = (uint8_t)(pins & (uint8_t)~0x20);
        }
      }
      return pins;
    }
    case 1:
    case 15:
      // PCR bit 0 clear: reading the A port clears the CA1 (vsync) flag.
      if ((via->pcr & 0x01) == 0) {
        via->ifr &= (uint8_t)~0x02;
      }
      if (system) {
        return SystemPortA(bbc, via);
      }
      return (uint8_t)((0xff & (uint8_t)~via->ddra) | (via->ora & via->ddra));
    case 2:
      return via->ddrb;
    case 3:
      return via->ddra;
    case 4:
      via->ifr &= (uint8_t)~0x40;
      return (uint8_t)(via->t1_counter & 0xff);
    case 5:
      return (uint8_t)((via->t1_counter >> 8) & 0xff);
    case 6:
      return via->t1l_l;
    case 7:
      return via->t1l_h;
    case 8:
      via->ifr &= (uint8_t)~0x20;
      return (uint8_t)(via->t2_counter & 0xff);
    case 9:
      return (uint8_t)((via->t2_counter >> 8) & 0xff);
    case 10:
      return via->sr;
    case 11:
      return via->acr;
    case 12:
      return via->pcr;
    case 13: {
      uint8_t ifr = via->ifr & 0x7f;
      if (ifr & via->ier) {
        ifr |= 0x80;
      }
      return ifr;
    }
    case 14:
      return (uint8_t)(via->ier | 0x80);
    default:
      return 0xfe;
  }
}

// CA2 tells the MOS whether the selected column has a key down. The scan
// clears the flag, writes the column, then reads the flag again, so the
// flag has to follow the column rather than the original key-down edge.
static bool ColumnHasKey(const BbcMachine* bbc, int column) {
  int row;
  if (column < 0 || column > 15) {
    return false;
  }
  for (row = 1; row < 8; row++) {
    if (bbc->key_down[column][row]) {
      return true;
    }
  }
  return false;
}

static void UpdateKeyboard(BbcMachine* bbc) {
  bool active = false;
  if (bbc->ic32 & 0x08) {
    int column;
    for (column = 0; column < 16; column++) {
      if (ColumnHasKey(bbc, column)) {
        active = true;
        break;
      }
    }
  } else {
    uint8_t pins = (uint8_t)((0xff & (uint8_t)~bbc->sys.ddra) | (bbc->sys.ora & bbc->sys.ddra));
    active = ColumnHasKey(bbc, pins & 0x0f);
  }
  if (active != bbc->ca2_high) {
    bbc->ca2_high = active;
    // PCR bit 2 is the CA2 active edge. MOS programs a positive edge.
    if (((bbc->sys.pcr & 0x04) != 0) == active) {
      bbc->sys.ifr |= 0x01;
    }
  }
}

static void PrinterStrobe(BbcMachine* bbc, uint8_t value) {
  uint8_t* grown;
  if (bbc->printed_len + 1 > bbc->printed_cap) {
    size_t cap = bbc->printed_cap == 0 ? 256 : bbc->printed_cap * 2;
    grown = realloc(bbc->printed, cap);
    if (grown == NULL) {
      return;
    }
    bbc->printed = grown;
    bbc->printed_cap = cap;
  }
  bbc->printed[bbc->printed_len++] = value;
  if (bbc->printer_path != NULL) {
    FILE* fp = fopen(bbc->printer_path, "ab");
    if (fp != NULL) {
      fputc(value, fp);
      fclose(fp);
    }
  }
  bbc->printer_ack = 64;
}

static void UserPrinterAck(BbcMachine* bbc) {
  // The printer acknowledges on CA1. The MOS programs the negative edge.
  if ((bbc->user.pcr & 0x01) == 0) {
    bbc->user.ifr |= 0x02;
  }
}

static void ViaWrite(BbcMachine* bbc, BbcVia* via, bool system, int reg, uint8_t value) {
  reg &= 0x0f;
  switch (reg) {
    case 0:
      via->orb = value;
      if (system) {
        uint8_t bit = (uint8_t)(value & 7);
        uint8_t data = (uint8_t)((value >> 3) & 1);
        uint8_t previous = bbc->ic32;
        if (data) {
          bbc->ic32 = (uint8_t)(bbc->ic32 | (uint8_t)(1u << bit));
        } else {
          bbc->ic32 = (uint8_t)(bbc->ic32 & (uint8_t)~(1u << bit));
        }
        // Sound-chip write enable is IC32 bit 0, active low.
        if ((previous & 1) && !(bbc->ic32 & 1)) {
          SoundWriteByte(bbc, via->ora);
        }
        CmosControl(bbc);
      }
      break;
    case 1:
    case 15:
      via->ora = value;
      break;
    case 2:
      via->ddrb = value;
      break;
    case 3:
      via->ddra = value;
      break;
    case 4:
    case 6:
      via->t1l_l = value;
      break;
    case 5:
      via->t1l_h = value;
      via->t1_counter = ((value << 8) | via->t1l_l) + 1;
      via->t1_running = true;
      via->ifr &= (uint8_t)~0x40;
      break;
    case 7:
      via->t1l_h = value;
      break;
    case 8:
      via->t2l_l = value;
      break;
    case 9:
      via->t2_counter = ((value << 8) | via->t2l_l) + 1;
      via->sr_period = via->t2_counter;
      via->t2_running = true;
      via->ifr &= (uint8_t)~0x20;
      break;
    case 10:
      via->sr = value;
      via->sr_bits = 0;
      via->ifr &= (uint8_t)~0x04;
      via->sr_active = ((via->acr >> 2) & 7) != 0;
      break;
    case 11:
      via->acr = value;
      // Free-run (ACR bit 6). Planetoid calls the user 6522 IC 69 and
      // does ?&FE6B=&C0:IF ?&FE64=?&FE64. A chip that has never had T1
      // loaded still counts once free-run is selected. Empty latches
      // would reload as a one-tick period, so the first span is a full
      // 16-bit count and the low byte changes between the two peeks.
      if ((value & 0x40) != 0 && !via->t1_running) {
        int period = ((via->t1l_h << 8) | via->t1l_l) + 1;
        via->t1_counter = period > 1 ? period : 0xffff;
        via->t1_running = true;
      }
      break;
    case 12: {
      int previous = (via->pcr >> 1) & 7;
      int next = (value >> 1) & 7;
      via->pcr = value;
      // CA2 manual-low is the Centronics strobe. Capture the data byte then.
      if (!system && previous != 6 && next == 6) {
        PrinterStrobe(bbc, via->ora);
      }
      break;
    }
    case 13:
      via->ifr &= (uint8_t)~(value & 0x7f);
      break;
    case 14:
      if (value & 0x80) {
        via->ier |= (uint8_t)(value & 0x7f);
      } else {
        via->ier &= (uint8_t)~(value & 0x7f);
      }
      break;
    default:
      break;
  }
  if (system && (reg == 0 || reg == 1 || reg == 3 || reg == 15)) {
    UpdateKeyboard(bbc);
  }
}

static bool ViaIrq(const BbcVia* via) {
  return (via->ifr & via->ier & 0x7f) != 0;
}

static void WriteControl(BbcMachine* bbc, uint8_t value) {
  bbc->ula_control = value;
}

static void WritePalette(BbcMachine* bbc, uint8_t value) {
  bbc->ula_pal[(value >> 4) & 0x0f] = (uint8_t)(value & 0x0f);
}

static int UlaMode(const BbcMachine* bbc) {
  return (bbc->ula_control >> 2) & 3;
}

static int PixelsPerChar(const BbcMachine* bbc) {
  return (bbc->ula_control & 0x10) ? 8 : 16;
}

// Interlace video steps the scanline by two, so a MODE 7 row is 10 lines,
// not R9+1. The snapshot renderer has to use the same count as the beam or
// the window changes shape whenever the clock drops back to real time.
static int DisplayScanlines(const BbcMachine* bbc) {
  int r9 = bbc->crtc[9] & 0x1f;
  int count;
  int sl;
  int guard;
  if ((bbc->crtc[8] & 3) != 3) {
    return r9 + 1;
  }
  count = 0;
  sl = 0;
  for (guard = 0; guard < 32; guard++) {
    count++;
    if (sl == r9) {
      return count;
    }
    sl = (sl + 2) & 0x1e;
  }
  return count > 0 ? count : 1;
}

static bool Teletext(const BbcMachine* bbc) {
  return (bbc->ula_control & 0x02) != 0;
}

static void PaletteRgb(const BbcMachine* bbc, int index, uint8_t rgb[3]) {
  uint8_t pal = bbc->ula_pal[index & 15];
  int colour = pal ^ 7;
  if ((pal & 8) && (bbc->ula_control & 1)) {
    colour = pal;
  }
  colour &= 7;
  rgb[0] = kRgb[colour][0];
  rgb[1] = kRgb[colour][1];
  rgb[2] = kRgb[colour][2];
}

static int PixelIndex(int ula_mode, uint8_t byte, int pixel) {
  int src = pixel >> (3 - ula_mode);
  unsigned temp = byte;
  int i;
  for (i = 0; i < src; i++) {
    temp = (temp << 1) | 1;
  }
  int left = 0;
  if (temp & 2) left |= 1;
  if (temp & 8) left |= 2;
  if (temp & 32) left |= 4;
  if (temp & 128) left |= 8;
  return left;
}

static void PutPixel(BbcMachine* bbc, int x, int y, const uint8_t rgb[3]) {
  if (x < 0 || y < 0 || x >= BBC_FB_WIDTH || y >= BBC_FB_HEIGHT) {
    return;
  }
  uint8_t* p = bbc->fb + ((size_t)y * BBC_FB_WIDTH + (size_t)x) * 3;
  p[0] = rgb[0];
  p[1] = rgb[1];
  p[2] = rgb[2];
}

static uint8_t VideoByte(const BbcMachine* bbc, uint16_t ma, uint8_t ra) {
  uint16_t addr;
  bool shadow;
  if (bbc->ram == NULL) {
    return 0;
  }
  addr = BbcVideoAddress(bbc, ma, ra);
  // ACCCON bit D selects LYNNE for the CRTC without moving it into the CPU map.
  shadow = bbc->master && (bbc->acccon & 0x01) != 0 && addr >= 0x3000 && addr <= 0x7fff;
  if (bbc->lynne == NULL || shadow == bbc->lynne_mapped || addr < 0x3000 || addr > 0x7fff) {
    return bbc->ram[addr];
  }
  return bbc->lynne[addr - 0x3000];
}

static bool CursorBlinkOn(const BbcMachine* bbc) {
  int mode = (bbc->crtc[10] >> 5) & 3;
  if (mode == 1) {
    return false;
  }
  if (mode == 2) {
    return (bbc->frames & 8) != 0;
  }
  if (mode == 3) {
    return (bbc->frames & 16) != 0;
  }
  return true;
}

static void InvertBlock(BbcMachine* bbc, int x, int y, int w, int h) {
  int yy, xx;
  for (yy = 0; yy < h; yy++) {
    for (xx = 0; xx < w; xx++) {
      int px = x + xx;
      int py = y + yy;
      if (px < 0 || py < 0 || px >= BBC_FB_WIDTH || py >= BBC_FB_HEIGHT) {
        continue;
      }
      uint8_t* p = bbc->fb + ((size_t)py * BBC_FB_WIDTH + (size_t)px) * 3;
      p[0] = (uint8_t)~p[0];
      p[1] = (uint8_t)~p[1];
      p[2] = (uint8_t)~p[2];
    }
  }
}

static int DisplayRowForScanline(const BbcMachine* bbc, int scanline) {
  int r9;
  int row;
  int sl;
  int guard;
  if ((bbc->crtc[8] & 3) != 3) {
    return scanline;
  }
  r9 = bbc->crtc[9] & 0x1f;
  row = 0;
  sl = 0;
  for (guard = 0; guard < 32; guard++) {
    if (sl == (scanline & 0x1f)) {
      return row;
    }
    if (sl == r9) {
      break;
    }
    sl = (sl + 2) & 0x1e;
    row++;
  }
  return scanline;
}

static void DrawCursorBar(BbcMachine* bbc, uint16_t ma, int x, int y, int w, int scanlines) {
  int start;
  int end;
  if (!CursorBlinkOn(bbc)) {
    return;
  }
  if (ma != (uint16_t)(((bbc->crtc[14] << 8) | bbc->crtc[15]) & 0x3fff)) {
    return;
  }
  start = DisplayRowForScanline(bbc, bbc->crtc[10] & 0x1f);
  end = DisplayRowForScanline(bbc, bbc->crtc[11] & 0x1f);
  if (start >= scanlines || start > end) {
    return;
  }
  if (end >= scanlines) {
    end = scanlines - 1;
  }
  // Two framebuffer lines per character row, so the underline covers both.
  InvertBlock(bbc, x, y + start * 2, w, (end - start + 1) * 2);
}

static void FillCell(BbcMachine* bbc, int x, int y, int w, int h, const uint8_t rgb[3]) {
  int yy, xx;
  for (yy = 0; yy < h; yy++) {
    for (xx = 0; xx < w; xx++) {
      PutPixel(bbc, x + xx, y + yy, rgb);
    }
  }
}

static bool FlashOn(const BbcMachine* bbc) {
  return ((bbc->frames / 25) & 1) == 0;
}

static void TeletextResetLine(BbcMachine* bbc) {
  bbc->tt_fg = 7;
  bbc->tt_bg = 0;
  bbc->tt_graphics = false;
  bbc->tt_separated = false;
  bbc->tt_conceal = false;
  bbc->tt_flash = false;
  bbc->tt_double = false;
  bbc->tt_hold = false;
  bbc->tt_held_valid = false;
}

static void TeletextApply(BbcMachine* bbc, uint8_t byte) {
  if (byte >= 0x01 && byte <= 0x07) {
    bbc->tt_graphics = false;
    bbc->tt_conceal = false;
    bbc->tt_held_valid = false;
    bbc->tt_fg = byte;
  } else if (byte == 0x08) {
    bbc->tt_flash = true;
  } else if (byte == 0x09) {
    bbc->tt_flash = false;
  } else if (byte == 0x0c) {
    bbc->tt_double = false;
  } else if (byte == 0x0d) {
    bbc->tt_double = true;
    bbc->tt_row_double = true;
  } else if (byte >= 0x11 && byte <= 0x17) {
    bbc->tt_graphics = true;
    bbc->tt_conceal = false;
    bbc->tt_fg = byte & 7;
  } else if (byte == 0x18) {
    bbc->tt_conceal = true;
  } else if (byte == 0x19) {
    bbc->tt_separated = false;
  } else if (byte == 0x1a) {
    bbc->tt_separated = true;
  } else if (byte == 0x1c) {
    bbc->tt_bg = 0;
  } else if (byte == 0x1d) {
    bbc->tt_bg = bbc->tt_fg;
  } else if (byte == 0x1e) {
    bbc->tt_hold = true;
  } else if (byte == 0x1f) {
    bbc->tt_hold = false;
    bbc->tt_held_valid = false;
  }
}

// One SAA5050 row is five dots, each shown as two half-dots, plus a blank
// dot of spacing on the left. Bit 11 of the result is the leftmost half-dot.
static uint16_t TeletextExpand(uint8_t bits) {
  bits = (uint8_t)(bits & 0x1f);
  return (uint16_t)(((bits & 0x01) * 0x03) + ((bits & 0x02) * 0x06) + ((bits & 0x04) * 0x0c) +
                    ((bits & 0x08) * 0x18) + ((bits & 0x10) * 0x30));
}

// US national option for the codes the UK chip draws as fractions and arrows.
// Five dots, bit 4 on the left, same as kSaa5050Uk.
static uint8_t UsTeletext(uint8_t glyph, int row) {
  static const unsigned char rows[][10] = {
      {0x23, 012, 012, 037, 012, 037, 012, 012, 000, 000},
      {0x5b, 016, 010, 010, 010, 010, 010, 016, 000, 000},
      {0x5c, 020, 010, 010, 004, 004, 002, 001, 000, 000},
      {0x5d, 016, 002, 002, 002, 002, 002, 016, 000, 000},
      {0x5e, 004, 012, 021, 000, 000, 000, 000, 000, 000},
      {0x5f, 000, 000, 000, 000, 000, 000, 037, 000, 000},
      {0x60, 010, 004, 000, 000, 000, 000, 000, 000, 000},
      {0x7b, 006, 010, 004, 014, 004, 010, 006, 000, 000},
      {0x7c, 004, 004, 004, 004, 004, 004, 004, 000, 000},
      {0x7d, 014, 002, 004, 006, 004, 002, 014, 000, 000},
      {0x7e, 000, 000, 010, 025, 002, 000, 000, 000, 000},
  };
  size_t i;
  for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    if (rows[i][0] == glyph) {
      return rows[i][1 + row];
    }
  }
  return kSaa5050Uk[glyph - 0x20][row];
}

static uint8_t TeletextMatrix(uint8_t glyph, int row, int keyboard) {
  if (glyph < 0x20 || glyph > 0x7f || row < 0 || row > 8) {
    return 0;
  }
  if (keyboard == BBC_KEYBOARD_US) {
    return UsTeletext(glyph, row);
  }
  return kSaa5050Uk[glyph - 0x20][row];
}

// A diagonal in the matrix inserts a half-dot beside the current row's dot.
static uint16_t TeletextRound(uint16_t current, uint16_t neighbor) {
  return (uint16_t)(current | ((current >> 1) & neighbor & ~(neighbor >> 1)) |
                    ((current << 1) & neighbor & ~(neighbor << 1)));
}

static int TeletextLineIndex(int line, int span) {
  int index;
  if (span < 1) {
    span = 1;
  }
  if (line < 0) {
    line = 0;
  }
  index = line * 10 / span;
  if (index > 9) {
    return 9;
  }
  return index;
}

// ra is the 0..19 rounding line. Out of range is a blank row.
static int TeletextRa(int row, int lower, bool doubled, bool bottom) {
  int ra;
  if (row < 0 || row > 9) {
    return -1;
  }
  ra = row * 2 + (lower ? 1 : 0);
  if (doubled) {
    ra /= 2;
    if (bottom) {
      ra += 10;
    }
  }
  return ra;
}

static uint16_t TeletextRaw(uint8_t glyph, int ra, int keyboard) {
  int neighbor;
  if (ra < 0 || ra > 19) {
    return 0;
  }
  neighbor = ra + ((ra & 1) ? 1 : -1);
  if (neighbor < 0) {
    neighbor = -1;
  } else {
    neighbor >>= 1;
  }
  return TeletextRound(TeletextExpand(TeletextMatrix(glyph, ra >> 1, keyboard)),
                       TeletextExpand(TeletextMatrix(glyph, neighbor, keyboard)));
}

// Each of the ten character rows is two framebuffer lines. The top line rounds
// against the row above and the bottom line against the row below.
static uint16_t TeletextPattern(uint8_t glyph, int row, int lower, bool doubled, bool bottom,
                                int keyboard) {
  return TeletextRaw(glyph, TeletextRa(row, lower, doubled, bottom), keyboard);
}

// The chip's rounded glyph is 12 half-dots wide. Drawing those one-to-one,
// centered in the cell, keeps the curves. Stretching them to fill all 16
// pixels makes some columns wider than others and turns a 0 into a spike.
static bool TeletextPixel(uint16_t pattern, int pix, int ppc) {
  int origin;
  int half;
  if (ppc < 1 || pix < 0) {
    return false;
  }
  origin = (ppc - 12) / 2;
  half = pix - origin;
  if (half < 0 || half > 11) {
    return false;
  }
  return (pattern & (uint16_t)(1u << (11 - half))) != 0;
}

static void DrawTeletext(BbcMachine* bbc, int cols, int rows, int scanlines, int ppc) {
  int row;
  bool bottom = false;
  uint16_t ma = (uint16_t)(((bbc->crtc[12] << 8) | bbc->crtc[13]) & 0x3fff);
  for (row = 0; row < rows; row++) {
    int col;
    TeletextResetLine(bbc);
    bbc->tt_bottom = bottom;
    bbc->tt_row_double = false;
    for (col = 0; col < cols; col++) {
      uint8_t byte = VideoByte(bbc, ma, 0) & 0x7f;
      int x = col * ppc;
      int y = row * scanlines * 2;
      bool control = byte < 0x20;
      uint8_t glyph = byte;
      // Codes 0x40-0x5F stay letters in graphics mode. Only bit 5 selects a mosaic.
      bool graphic = bbc->tt_graphics && !control && (glyph & 0x20) != 0;
      bool hidden = bbc->tt_conceal || (bbc->tt_flash && !FlashOn(bbc));
      int fg = bbc->tt_fg & 7;
      int bg = bbc->tt_bg & 7;
      if (control && bbc->tt_hold && bbc->tt_graphics && bbc->tt_held_valid) {
        glyph = bbc->tt_held;
        graphic = true;
        control = false;
      }
      if (control || hidden || glyph < 0x20) {
        FillCell(bbc, x, y, ppc, scanlines * 2, kRgb[bg]);
      } else if (graphic) {
        static const uint8_t kBit[6] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x40};
        int sy;
        int vspan = bbc->tt_double ? scanlines * 2 : scanlines;
        int origin = bbc->tt_double && bbc->tt_bottom ? scanlines : 0;
        for (sy = 0; sy < scanlines; sy++) {
          int band = ((origin + sy) * 3) / (vspan > 0 ? vspan : 1);
          int half;
          if (band > 2) {
            band = 2;
          }
          for (half = 0; half < 2; half++) {
            int yy = y + sy * 2 + half;
            int s;
            for (s = 0; s < 2; s++) {
              int which = band * 2 + s;
              const uint8_t* colour = (glyph & kBit[which]) ? kRgb[fg] : kRgb[bg];
              int x0 = x + s * (ppc / 2);
              int w = ppc / 2;
              if (bbc->tt_separated) {
                FillCell(bbc, x0, yy, w, 1, kRgb[bg]);
                if (w > 2) {
                  FillCell(bbc, x0 + 1, yy, w - 2, 1, colour);
                }
              } else {
                FillCell(bbc, x0, yy, w, 1, colour);
              }
            }
          }
        }
      } else {
        int sy;
        for (sy = 0; sy < scanlines; sy++) {
          int index = TeletextLineIndex(sy, scanlines);
          int half;
          for (half = 0; half < 2; half++) {
            uint16_t pattern =
                TeletextPattern(glyph, index, half, bbc->tt_double, bbc->tt_bottom,
                                bbc->keyboard);
            int yy = y + sy * 2 + half;
            int pix;
            for (pix = 0; pix < ppc; pix++) {
              const uint8_t* colour = TeletextPixel(pattern, pix, ppc) ? kRgb[fg] : kRgb[bg];
              PutPixel(bbc, x + pix, yy, colour);
            }
          }
        }
      }
      if (byte < 0x20) {
        TeletextApply(bbc, byte);
      } else if (bbc->tt_graphics && (byte & 0x20) != 0) {
        bbc->tt_held = byte;
        bbc->tt_held_valid = true;
      }
      DrawCursorBar(bbc, ma, x, y, ppc, scanlines);
      ma = (uint16_t)((ma + 1) & 0x3fff);
    }
    if (bbc->tt_row_double) {
      bottom = !bottom;
    } else {
      bottom = false;
    }
  }
}

static void DrawBitmap(BbcMachine* bbc, int cols, int rows, int scanlines, int ppc) {
  static const uint8_t kBlack[3] = {0, 0, 0};
  int ula = UlaMode(bbc);
  uint16_t cursor = (uint16_t)(((bbc->crtc[14] << 8) | bbc->crtc[15]) & 0x3fff);
  int cursor_start = bbc->crtc[10] & 0x1f;
  int cursor_end = bbc->crtc[11] & 0x1f;
  bool cursor_blink = CursorBlinkOn(bbc);
  uint16_t ma = (uint16_t)(((bbc->crtc[12] << 8) | bbc->crtc[13]) & 0x3fff);
  int row;
  int y = 0;
  for (row = 0; row < rows; row++) {
    int ra;
    uint16_t row_ma = ma;
    for (ra = 0; ra < scanlines; ra++) {
      ma = row_ma;
      int col;
      int x = 0;
      for (col = 0; col < cols; col++) {
        int pix;
        bool cursor_on = cursor_blink && ma == cursor && ra >= cursor_start &&
                         ra <= cursor_end;
        if (ra >= 8) {
          for (pix = 0; pix < ppc; pix++) {
            PutPixel(bbc, x + pix, y, kBlack);
          }
        } else {
          uint8_t byte = VideoByte(bbc, ma, (uint8_t)ra);
          for (pix = 0; pix < ppc; pix++) {
            uint8_t rgb[3];
            PaletteRgb(bbc, PixelIndex(ula, byte, pix), rgb);
            if (cursor_on) {
              rgb[0] = (uint8_t)~rgb[0];
              rgb[1] = (uint8_t)~rgb[1];
              rgb[2] = (uint8_t)~rgb[2];
            }
            PutPixel(bbc, x + pix, y, rgb);
          }
        }
        x += ppc;
        ma = (uint16_t)((ma + 1) & 0x3fff);
      }
      y++;
      if (y >= BBC_FB_HEIGHT) {
        return;
      }
    }
  }
}

void BbcMachineRender(BbcMachine* bbc) {
  int cols = bbc->crtc[1];
  int rows = bbc->crtc[6];
  int scanlines = DisplayScanlines(bbc);
  int ppc = PixelsPerChar(bbc);
  memset(bbc->fb, 0, (size_t)BBC_FB_WIDTH * BBC_FB_HEIGHT * 3);
  if (cols <= 0 || rows <= 0 || scanlines <= 0) {
    bbc->frame_width = 640;
    bbc->frame_height = 256;
    return;
  }
  if (cols > 80) {
    cols = 80;
  }
  if (ppc < 1) {
    ppc = 8;
  }
  if (cols * ppc > BBC_FB_WIDTH) {
    cols = BBC_FB_WIDTH / ppc;
  }
  // MODE 7 stores both rounding halves, so a character row is twice as many
  // lines as the CRTC scan. Bitmap modes stay one line per scan.
  {
    int scale = Teletext(bbc) ? 2 : 1;
    if (rows * scanlines * scale > BBC_FB_HEIGHT) {
      rows = BBC_FB_HEIGHT / (scanlines * scale);
    }
    bbc->frame_width = cols * ppc;
    bbc->frame_height = rows * scanlines * scale;
  }
  if (Teletext(bbc)) {
    DrawTeletext(bbc, cols, rows, scanlines, ppc);
  } else {
    DrawBitmap(bbc, cols, rows, scanlines, ppc);
  }
}

static void ProgramPalette(BbcMachine* bbc, const uint8_t* bytes, int count) {
  int i;
  for (i = 0; i < count; i++) {
    WritePalette(bbc, bytes[i]);
  }
}

void BbcMachineSelectMode(BbcMachine* bbc, int mode) {
  // MOS 1.20 CRTC tables. R12/R13 are the CRTC start, which bitmap modes
  // store as the RAM address divided by 8.
  static const uint8_t kCrtc[8][12] = {
      {0x7F, 0x50, 0x62, 0x28, 0x26, 0x00, 0x20, 0x22, 0x01, 0x07, 0x67, 0x08},
      {0x7F, 0x50, 0x62, 0x28, 0x26, 0x00, 0x20, 0x22, 0x01, 0x07, 0x67, 0x08},
      {0x7F, 0x50, 0x62, 0x28, 0x26, 0x00, 0x20, 0x22, 0x01, 0x07, 0x67, 0x08},
      {0x7F, 0x50, 0x62, 0x28, 0x1E, 0x02, 0x19, 0x1B, 0x01, 0x09, 0x67, 0x09},
      {0x3F, 0x28, 0x31, 0x24, 0x26, 0x00, 0x20, 0x22, 0x01, 0x07, 0x67, 0x08},
      {0x3F, 0x28, 0x31, 0x24, 0x26, 0x00, 0x20, 0x22, 0x01, 0x07, 0x67, 0x08},
      {0x3F, 0x28, 0x31, 0x24, 0x1E, 0x02, 0x19, 0x1B, 0x01, 0x09, 0x67, 0x09},
      {0x3F, 0x28, 0x33, 0x24, 0x1E, 0x02, 0x19, 0x1B, 0x93, 0x12, 0x72, 0x13},
  };
  static const uint8_t kUla[8] = {0x9C, 0xD8, 0xF4, 0x9C, 0x88, 0xC4, 0x88, 0x4B};
  static const uint16_t kStart[8] = {0x0600, 0x0600, 0x0600, 0x0800,
                                     0x0B00, 0x0B00, 0x0C00, 0x7C00};
  if (mode < 0 || mode > 7) {
    return;
  }
  memcpy(bbc->crtc, kCrtc[mode], 12);
  bbc->crtc[12] = (uint8_t)(kStart[mode] >> 8);
  bbc->crtc[13] = (uint8_t)(kStart[mode] & 0xff);
  WriteControl(bbc, kUla[mode]);
  if (mode == 2) {
    ProgramPalette(bbc, kPal16, 16);
  } else if (mode == 1 || mode == 5) {
    ProgramPalette(bbc, kPal4, 16);
  } else if (mode != 7) {
    ProgramPalette(bbc, kPal2, 16);
  }
}

static const int kUlaBaud[8] = {19200, 1200, 4800, 150, 9600, 300, 2400, 75};

static int CharCycles(int baud) {
  if (baud < 75) {
    baud = 1200;
  }
  return (BBC_CPU_HZ * 10) / baud;
}

static bool CassetteSelected(const BbcMachine* bbc) {
  return (bbc->serial_ula & 0x40) != 0;
}

static bool MotorOn(const BbcMachine* bbc) {
  return (bbc->serial_ula & 0x80) != 0;
}

static void SerialUpdate(BbcMachine* bbc) {
  bbc->acia_baud_tx = kUlaBaud[bbc->serial_ula & 7];
  bbc->acia_baud_rx = kUlaBaud[(bbc->serial_ula >> 3) & 7];
}

static bool AcaiIrq(const BbcMachine* bbc) {
  bool receive = (bbc->acia_control & 0x80) != 0;
  bool transmit = (bbc->acia_control & 0x60) == 0x20;
  if ((bbc->acia_control & 0x03) == 0x03) {
    return false;
  }
  return (receive && bbc->acia_rx_full) || (transmit && bbc->acia_tx_empty);
}

static uint8_t AcaiStatus(const BbcMachine* bbc) {
  uint8_t status = 0;
  if ((bbc->acia_control & 0x03) == 0x03) {
    return 0x02;
  }
  if (bbc->acia_rx_full) {
    status = (uint8_t)(status | 0x01);
  }
  if (bbc->acia_tx_empty) {
    status = (uint8_t)(status | 0x02);
  }
  if (bbc->acia_dcd) {
    status = (uint8_t)(status | 0x04);
  }
  if (bbc->acia_overrun) {
    status = (uint8_t)(status | 0x20);
  }
  if (AcaiIrq(bbc)) {
    status = (uint8_t)(status | 0x80);
  }
  return status;
}

static void TapeAppend(BbcMachine* bbc, uint8_t data, uint8_t dcd) {
  size_t cap = bbc->tape_len + 1;
  void* grown = realloc(bbc->tape, cap * sizeof(*bbc->tape));
  if (grown == NULL) {
    return;
  }
  bbc->tape = grown;
  bbc->tape[bbc->tape_len].data = data;
  bbc->tape[bbc->tape_len].dcd = dcd;
  bbc->tape_len++;
}

static void AcaiWriteControl(BbcMachine* bbc, uint8_t value) {
  bbc->acia_control = value;
  if ((value & 0x03) == 0x03) {
    bbc->acia_rx_full = false;
    bbc->acia_tx_empty = true;
    bbc->acia_overrun = false;
    bbc->acia_dcd = false;
    bbc->acia_rx_wait = 0;
    bbc->acia_tx_wait = 0;
  }
}

static void AcaiWriteData(BbcMachine* bbc, uint8_t value) {
  if ((bbc->acia_control & 0x03) == 0x03) {
    return;
  }
  bbc->acia_tx = value;
  bbc->acia_tx_empty = false;
  bbc->acia_tx_wait = CharCycles(bbc->acia_baud_tx);
  if (!CassetteSelected(bbc)) {
    uint8_t ch = value;
    if (write(1, &ch, 1) < 0) {
      // Host stdout is unavailable; the guest write is still consumed.
    }
  }
}

static void AcaiAdvance(BbcMachine* bbc, int cycles) {
  uint8_t ch;
  if ((bbc->acia_control & 0x03) == 0x03) {
    return;
  }
  if (bbc->acia_tx_wait > 0) {
    bbc->acia_tx_wait -= cycles;
    if (bbc->acia_tx_wait <= 0) {
      bbc->acia_tx_wait = 0;
      bbc->acia_tx_empty = true;
      // Recording only happens past the end of the tape. Leave the position
      // there, or the next byte is played back instead of recorded.
      if (CassetteSelected(bbc) && MotorOn(bbc) && !bbc->tape_protect &&
          bbc->tape_pos >= bbc->tape_len) {
        size_t before = bbc->tape_len;
        TapeAppend(bbc, bbc->acia_tx, 0);
        if (bbc->tape_len > before) {
          bbc->tape_pos = bbc->tape_len;
          bbc->tape_dirty = true;
        }
      }
    }
  }
  if (bbc->acia_rx_wait > 0) {
    bbc->acia_rx_wait -= cycles;
    if (bbc->acia_rx_wait < 0) {
      bbc->acia_rx_wait = 0;
    }
  }
  if (bbc->acia_rx_full || bbc->acia_rx_wait > 0) {
    return;
  }
  if (CassetteSelected(bbc) && MotorOn(bbc) && bbc->tape != NULL &&
      bbc->tape_pos < bbc->tape_len) {
    size_t origin = bbc->tape_pos;
    size_t run = 0;
    int carrier = bbc->tape[bbc->tape_pos].dcd != 0;
    if (!carrier && bbc->tape[bbc->tape_pos].data == 0xaa) {
      while (origin > 0 && bbc->tape[origin - 1].data == 0xaa && bbc->tape[origin - 1].dcd == 0) {
        origin--;
      }
      while (origin + run < bbc->tape_len && bbc->tape[origin + run].data == 0xaa &&
             bbc->tape[origin + run].dcd == 0) {
        run++;
      }
      carrier = (origin + run < bbc->tape_len && bbc->tape[origin + run].data == 0x2a) || run >= 8;
    }
    bbc->acia_rx = bbc->tape[bbc->tape_pos].data;
    bbc->acia_dcd = carrier;
    bbc->tape_pos++;
    bbc->acia_rx_full = true;
    bbc->acia_rx_wait = CharCycles(bbc->acia_baud_rx);
    return;
  }
  if (CassetteSelected(bbc)) {
    bbc->acia_dcd = false;
    return;
  }
  if (!HostStdinByte(&ch)) {
    return;
  }
  bbc->acia_rx = ch;
  bbc->acia_rx_full = true;
  bbc->acia_dcd = false;
}

static void AdcWrite(BbcMachine* bbc, uint8_t value) {
  bbc->adc_status = value;
  bbc->adc_busy = true;
  bbc->sys_cb1 = true;
  // Bit 3 clear is the short 8-bit conversion. Bit 3 set is the 10-bit one.
  bbc->adc_delay = (value & 0x08) ? 20000 : 8000;
}

static void AdcAdvance(BbcMachine* bbc, int cycles) {
  int channel;
  uint16_t sample;
  if (!bbc->adc_busy) {
    return;
  }
  bbc->adc_delay -= cycles;
  if (bbc->adc_delay > 0) {
    return;
  }
  bbc->adc_busy = false;
  channel = bbc->adc_status & 3;
  sample = bbc->analogue[channel];
  bbc->adc_hi = (uint8_t)(sample >> 8);
  bbc->adc_lo = (bbc->adc_status & 0x08) ? (uint8_t)(sample & 0xc0) : 0;
  if (bbc->sys_cb1 && (bbc->sys.pcr & 0x10) == 0) {
    bbc->sys.ifr |= 0x10;
  }
  bbc->sys_cb1 = false;
}

static uint8_t AdcRead(const BbcMachine* bbc, int offset) {
  offset &= 3;
  if (offset == 0) {
    uint8_t status = (uint8_t)(bbc->adc_status & 0x0f);
    if (!bbc->adc_busy) {
      status = (uint8_t)(status | 0x40);
    }
    return status;
  }
  if (offset == 1) {
    return bbc->adc_hi;
  }
  if (offset == 2) {
    return bbc->adc_lo;
  }
  return 0;
}

static bool EnsureMasterRam(BbcMachine* bbc) {
  if (bbc->lynne == NULL) {
    bbc->lynne = calloc(0x5000, 1);
  }
  if (bbc->hazel == NULL) {
    bbc->hazel = calloc(0x2000, 1);
  }
  if (bbc->andy == NULL) {
    bbc->andy = calloc(0x1000, 1);
  }
  if (bbc->mos_low == NULL) {
    bbc->mos_low = calloc(0x2000, 1);
  }
  return bbc->lynne != NULL && bbc->hazel != NULL && bbc->andy != NULL && bbc->mos_low != NULL;
}

static bool SidewaysActive(const BbcMachine* bbc);

bool BbcMachineSidewaysRom(const BbcMachine* bbc, uint16_t addr) {
  if (bbc == NULL || addr < 0x8000 || addr > 0xbfff || !SidewaysActive(bbc)) {
    return false;
  }
  // ANDY is RAM at &8000-&8FFF while ROMSEL bit 7 is set.
  if (bbc->master && bbc->andy_mapped && addr < 0x9000) {
    return false;
  }
  // Bits 4-6 select a virtual image. Those images are ROM.
  if (((bbc->romsel >> 4) & 7) != 0) {
    return true;
  }
  return !bbc->sideways_ram[bbc->romsel & 0x0f];
}

bool BbcMachineMosRom(const BbcMachine* bbc, uint16_t addr) {
  if (bbc == NULL || addr < 0xc000 || (addr >= 0xfc00 && addr <= 0xfeff)) {
    return false;
  }
  if (bbc->master && addr <= 0xdfff) {
    return !bbc->hazel_mapped;
  }
  return true;
}

static bool SidewaysActive(const BbcMachine* bbc) {
  int i;
  if (bbc->master || bbc->any_sideways) {
    return true;
  }
  for (i = 0; i < 16; i++) {
    if (bbc->sideways_ram[i]) {
      return true;
    }
  }
  return false;
}

static void SwapBytes(uint8_t* a, uint8_t* b, size_t length) {
  uint8_t tmp[256];
  while (length > 0) {
    size_t n = length > sizeof(tmp) ? sizeof(tmp) : length;
    memcpy(tmp, a, n);
    memcpy(a, b, n);
    memcpy(b, tmp, n);
    a += n;
    b += n;
    length -= n;
  }
}

static bool LynneForCpu(const BbcMachine* bbc) {
  if ((bbc->acccon & 0x04) != 0) {
    return true;
  }
  if ((bbc->acccon & 0x02) != 0 && bbc->last_pc >= 0xc000 && bbc->last_pc <= 0xdfff) {
    return true;
  }
  return false;
}

static void ApplyLynne(BbcMachine* bbc, bool on) {
  if (bbc->ram == NULL || bbc->lynne == NULL || on == bbc->lynne_mapped) {
    return;
  }
  SwapBytes(bbc->ram + 0x3000, bbc->lynne, 0x5000);
  bbc->lynne_mapped = on;
}

static void ApplyHazel(BbcMachine* bbc, bool on) {
  if (bbc->ram == NULL || bbc->hazel == NULL || bbc->mos_low == NULL || on == bbc->hazel_mapped) {
    return;
  }
  if (on) {
    memcpy(bbc->ram + 0xc000, bbc->hazel, 0x2000);
  } else {
    memcpy(bbc->hazel, bbc->ram + 0xc000, 0x2000);
    memcpy(bbc->ram + 0xc000, bbc->mos_low, 0x2000);
  }
  bbc->hazel_mapped = on;
}

static void CommitSideways(BbcMachine* bbc) {
  int slot = bbc->mapped_slot;
  // A virtual image is ROM. Leave it, and leave the RAM image, unchanged.
  if (bbc->mapped_virt != 0) {
    return;
  }
  if (bbc->ram == NULL || slot < 0 || slot > 15 || !bbc->sideways_ram[slot] ||
      bbc->sideways[slot] == NULL) {
    return;
  }
  // ANDY occupies &8000-&8FFF, so a RAM bank only owns the rest of the window.
  if (bbc->andy_mapped) {
    memcpy(bbc->sideways[slot] + 0x1000, bbc->ram + 0x9000, 0x3000);
  } else {
    memcpy(bbc->sideways[slot], bbc->ram + 0x8000, 16384);
  }
}

static uint8_t* SidewaysImage(const BbcMachine* bbc, int slot, int virt) {
  if (virt <= 0) {
    return bbc->sideways[slot];
  }
  return bbc->sideways_page[slot][virt - 1];
}

static void MapRomsel(BbcMachine* bbc) {
  int slot;
  int virt;
  bool andy;
  bool have;
  uint8_t* image;
  if (bbc->ram == NULL) {
    return;
  }
  slot = bbc->romsel & 0x0f;
  virt = (bbc->romsel >> 4) & 7;
  andy = bbc->master && (bbc->romsel & 0x80) != 0;
  if (!SidewaysActive(bbc)) {
    bbc->mapped_slot = slot;
    bbc->mapped_virt = virt;
    bbc->andy_mapped = false;
    return;
  }
  if (slot == bbc->mapped_slot && virt == bbc->mapped_virt && andy == bbc->andy_mapped) {
    return;
  }
  CommitSideways(bbc);
  if (bbc->andy_mapped && !andy && bbc->andy != NULL) {
    memcpy(bbc->andy, bbc->ram + 0x8000, 0x1000);
  }
  if (andy && !bbc->andy_mapped && bbc->andy != NULL) {
    memcpy(bbc->ram + 0x8000, bbc->andy, 0x1000);
  }
  bbc->andy_mapped = andy;
  bbc->mapped_slot = slot;
  bbc->mapped_virt = virt;
  image = SidewaysImage(bbc, slot, virt);
  if (virt == 0) {
    have = image != NULL && (bbc->sideways_loaded[slot] || bbc->sideways_ram[slot]);
  } else {
    have = image != NULL;
  }
  if (have) {
    if (andy) {
      memcpy(bbc->ram + 0x9000, image + 0x1000, 0x3000);
    } else {
      memcpy(bbc->ram + 0x8000, image, 16384);
    }
  } else if (andy) {
    memset(bbc->ram + 0x9000, 0xff, 0x3000);
  } else {
    memset(bbc->ram + 0x8000, 0xff, 16384);
  }
}

void BbcMachineMapSideways(BbcMachine* bbc) { MapRomsel(bbc); }

// ADFS 1.30's map checksum starts from &FF, so a cleared map is only valid
// when both checksum bytes are &FF. The clear stores zeros, and the next
// OSFIND then raises "Bad FS map". Beebug C's Escape handler closes files
// and treats that as another error, so the two calls nest until the stack
// wraps. A map that has never been read from a disc is this all-zero page.
static void AdfsNoteClearedMap(BbcMachine* bbc) {
  int i;
  if (bbc->ram == NULL) {
    return;
  }
  if (bbc->ram[0x848c] != 0x99 || bbc->ram[0x848d] != 0x00 || bbc->ram[0x848e] != 0x0f ||
      bbc->ram[0x848f] != 0x99 || bbc->ram[0x8490] != 0x00 || bbc->ram[0x8491] != 0x0e) {
    return;
  }
  for (i = 0; i < 255; i++) {
    if (bbc->ram[0x0e00 + i] != 0 || bbc->ram[0x0f00 + i] != 0) {
      return;
    }
  }
  bbc->ram[0x0eff] = 0xff;
  bbc->ram[0x0fff] = 0xff;
}

void BbcMachineBeginInstruction(BbcMachine* bbc, uint16_t pc) {
  if (bbc == NULL) {
    return;
  }
  if (pc == 0x8498) {
    AdfsNoteClearedMap(bbc);
  }
  if (!bbc->master) {
    return;
  }
  bbc->last_pc = pc;
  ApplyLynne(bbc, LynneForCpu(bbc));
}

// ACCCON bit TST makes the OS ROM readable through &FC00-&FEFF, where the
// 1 MHz bus and SHEILA normally sit. MOS 3.50 enters at &FC00 with TST set.
static void ApplyTst(BbcMachine* bbc, bool on) {
  if (bbc->ram == NULL || bbc->os_rom == NULL) {
    return;
  }
  if (on) {
    memcpy(bbc->ram + 0xfc00, bbc->os_rom + 0x3c00, 0x300);
  } else {
    memset(bbc->ram + 0xfc00, 0xff, 0x300);
  }
}

static void WriteAcccon(BbcMachine* bbc, uint8_t value) {
  uint8_t old = bbc->acccon;
  bbc->acccon = value;
  if (((old ^ value) & 0x08) != 0) {
    ApplyHazel(bbc, (value & 0x08) != 0);
  }
  if (((old ^ value) & 0x06) != 0) {
    ApplyLynne(bbc, LynneForCpu(bbc));
  }
  if (((old ^ value) & 0x40) != 0) {
    ApplyTst(bbc, (value & 0x40) != 0);
  }
}

void BbcMachineBreak(BbcMachine* bbc) {
  if (bbc == NULL) {
    return;
  }
  if (bbc->master) {
    WriteAcccon(bbc, 0);
  }
  bbc->romsel = 0;
  bbc->hd.phase = 0;
  MapRomsel(bbc);
  if (bbc->parasite_reset != NULL) {
    bbc->parasite_reset(bbc->parasite);
  }
}

void BbcMachineCaptureMos(BbcMachine* bbc) {
  if (bbc == NULL || !bbc->master || bbc->ram == NULL || bbc->mos_low == NULL || bbc->hazel_mapped) {
    return;
  }
  memcpy(bbc->mos_low, bbc->ram + 0xc000, 0x2000);
  bbc->mos_saved = true;
}

// The Model B floppy socket is either an Intel 8271 (DFS 0.90/1.20) or a
// WD1770 (1770 DFS and ADFS). Both raise NMI once per transferred byte.
#define I8_BUSY 0x80
#define I8_RESULT 0x10
#define I8_INT 0x08
#define I8_NDM 0x04
#define WD_MOTOR 0x80
#define WD_WP 0x40
#define WD_RNF 0x10
#define WD_TRACK0 0x04
#define WD_DRQ 0x02
#define WD_BUSY 0x01

enum {
  FDC_IDLE = 0,
  FDC_READ = 1,
  FDC_READ_ID = 2,
  FDC_WRITE = 3,
  FDC_WRITE_WAIT = 4,
  FDC_VERIFY = 5,
  FDC_FINISH = 6,
  FDC_READ_TRACK = 7,
  FDC_WRITE_TRACK = 8,
};

static bool Fdc1770(const BbcMachine* bbc) {
  return bbc->fdc.kind == BBC_FDC_1770;
}

static void UpdateNmiLine(BbcMachine* bbc) {
  bool on = bbc->fdc.nmi_line || (bbc->econet.fitted && bbc->econet.nmi_enable && bbc->econet.irq);
  if (on && !bbc->nmi_line) {
    bbc->nmi_edge = true;
  }
  bbc->nmi_line = on;
}

static void FdcSetLine(BbcMachine* bbc, bool on) {
  bbc->fdc.nmi_line = on;
  UpdateNmiLine(bbc);
}

static void FdcUpdateNmi(BbcMachine* bbc) {
  bool on;
  if (Fdc1770(bbc)) {
    on = bbc->fdc.intrq || (bbc->fdc.status & WD_DRQ) != 0;
  } else {
    on = (bbc->fdc.status & I8_INT) != 0;
  }
  FdcSetLine(bbc, on);
}

// The surface the controller is about to touch. side_in_image is the head
// inside that image: 0 for a disc mounted as DFS drive 2 or 3.
static BbcDiscImage* FdcImage(const BbcMachine* bbc, int* side_in_image) {
  BbcMachine* machine = (BbcMachine*)bbc;
  int drive = machine->fdc.drive;
  int side = machine->fdc.side;
  if (side_in_image != NULL) {
    *side_in_image = 0;
  }
  if (drive < 0 || drive > 1 || side < 0) {
    return NULL;
  }
  if (side >= 1 && machine->fdc.extra[drive].data != NULL) {
    return &machine->fdc.extra[drive];
  }
  if (machine->fdc.disc[drive].data == NULL || side >= machine->fdc.disc[drive].sides) {
    return NULL;
  }
  if (side_in_image != NULL) {
    *side_in_image = side;
  }
  return &machine->fdc.disc[drive];
}

static bool DriveHasDisc(const BbcMachine* bbc, int drive) {
  return drive >= 0 && drive <= 1 &&
         (bbc->fdc.disc[drive].data != NULL || bbc->fdc.extra[drive].data != NULL);
}

static bool FdcNoDisc(const BbcMachine* bbc) {
  return FdcImage(bbc, NULL) == NULL;
}

static uint8_t FdcOk(const BbcMachine* bbc) {
  return Fdc1770(bbc) ? WD_MOTOR : 0;
}

static uint8_t FdcMissing(const BbcMachine* bbc) {
  if (Fdc1770(bbc)) {
    return (uint8_t)(WD_MOTOR | WD_RNF);
  }
  return FdcNoDisc(bbc) ? 0x10 : 0x18;
}

static uint8_t FdcProtected(const BbcMachine* bbc) {
  return Fdc1770(bbc) ? (uint8_t)(WD_MOTOR | WD_WP) : 0x12;
}

static void FdcFinish(BbcMachine* bbc, uint8_t code) {
  bbc->fdc.phase = FDC_IDLE;
  bbc->fdc.delay = 0;
  if (Fdc1770(bbc)) {
    bbc->fdc.status = code;
    bbc->fdc.intrq = true;
  } else {
    bbc->fdc.result = code;
    bbc->fdc.status = (uint8_t)(I8_RESULT | I8_INT);
  }
  FdcUpdateNmi(bbc);
}

static void FdcAsk(BbcMachine* bbc, bool with_data, uint8_t byte) {
  if (with_data) {
    bbc->fdc.data = byte;
  }
  if (Fdc1770(bbc)) {
    bbc->fdc.intrq = false;
    bbc->fdc.status = (uint8_t)(WD_MOTOR | WD_BUSY | WD_DRQ);
  } else {
    bbc->fdc.status = (uint8_t)(I8_BUSY | I8_INT | I8_NDM);
  }
  FdcUpdateNmi(bbc);
}

static void FdcAck(BbcMachine* bbc) {
  if (Fdc1770(bbc)) {
    bbc->fdc.status = (uint8_t)(bbc->fdc.status & (uint8_t)~WD_DRQ);
  } else {
    bbc->fdc.status = (uint8_t)(bbc->fdc.status & (uint8_t)~(I8_INT | I8_NDM));
  }
  FdcUpdateNmi(bbc);
}

static void FdcLatchSide(BbcMachine* bbc) {
  if (bbc->master) {
    // &FE24 bit 2 resets the 1770. The side is bit 4, unlike the Model B latch.
    bbc->fdc.side = (bbc->fdc.control & 0x10) ? 1 : 0;
  } else if (Fdc1770(bbc)) {
    bbc->fdc.side = (bbc->fdc.control & 0x04) ? 1 : 0;
  } else {
    bbc->fdc.side = (bbc->fdc.drvout & 0x20) ? 1 : 0;
  }
}

static int FdcSectorOff(const BbcMachine* bbc, int size) {
  int side = 0;
  BbcDiscImage* image = FdcImage(bbc, &side);
  int track;
  size_t off;
  if (image == NULL || size < 1) {
    return -1;
  }
  track = Fdc1770(bbc) ? bbc->fdc.track_reg : bbc->fdc.params[0];
  if (track < 0 || track >= image->tracks || side < 0 || side >= image->sides ||
      bbc->fdc.sector < 0 || bbc->fdc.sector >= image->spt) {
    return -1;
  }
  off = (((size_t)track * (size_t)image->sides + (size_t)side) * (size_t)image->spt +
         (size_t)bbc->fdc.sector) *
        256;
  (void)size;
  return (int)off;
}

// A short image can end in the middle of a track. Bytes past the file are
// empty rather than a missing sector.
static uint8_t DiscByte(const BbcDiscImage* image, int off) {
  if (image == NULL || image->data == NULL || off < 0 || (size_t)off >= image->length) {
    return 0;
  }
  return image->data[off];
}

static void DiscFlushImage(BbcDiscImage* image) {
  FILE* fp;
  if (image == NULL || image->protect || image->data == NULL) {
    return;
  }
  if (image->fd >= 0) {
    if (CumanaSendImage(image->fd, image->data, image->length) != 0) {
      SocketClose(image->fd);
      image->fd = -1;
    }
    return;
  }
  if (image->path == NULL) {
    return;
  }
  fp = fopen(image->path, "r+b");
  if (fp == NULL) {
    return;
  }
  if (fwrite(image->data, 1, image->length, fp) != image->length) {
    fclose(fp);
    return;
  }
  fclose(fp);
}

static void FdcBegin(BbcMachine* bbc, int phase, int sector, int count, int size) {
  FdcLatchSide(bbc);
  if (count < 1) {
    count = 1;
  }
  if (size < 0) {
    size = 0;
  }
  if (size > 1024) {
    size = 1024;
  }
  bbc->fdc.sector = sector;
  bbc->fdc.sectors_left = count;
  bbc->fdc.sector_size = size;
  bbc->fdc.bytes_left = size;
  bbc->fdc.sector_off = -1;
  bbc->fdc.buf_i = 0;
  bbc->fdc.writing = phase == FDC_WRITE;
  bbc->fdc.phase = phase;
  // On the BBC the 1770 density bit is active low: clear selects MFM.
  bbc->fdc.byte_gap = (Fdc1770(bbc) && (bbc->fdc.control & 0x08) == 0) ? 64 : 128;
  bbc->fdc.delay = 400;
  if (Fdc1770(bbc)) {
    bbc->fdc.intrq = false;
    bbc->fdc.status = (uint8_t)(WD_MOTOR | WD_BUSY);
  } else {
    bbc->fdc.status = I8_BUSY;
  }
  FdcUpdateNmi(bbc);
}

static void FdcService(BbcMachine* bbc) {
  int off;
  int size;
  if (bbc->fdc.phase == FDC_FINISH) {
    FdcFinish(bbc, bbc->fdc.result);
    return;
  }
  if (bbc->fdc.phase == FDC_READ_TRACK) {
    if (bbc->fdc.buf_i >= bbc->fdc.bytes_left) {
      FdcFinish(bbc, FdcOk(bbc));
      return;
    }
    FdcAsk(bbc, true, bbc->fdc.track_image[bbc->fdc.buf_i]);
    bbc->fdc.buf_i++;
    return;
  }
  if (bbc->fdc.phase == FDC_WRITE_TRACK) {
    FdcAsk(bbc, false, 0);
    return;
  }
  if (bbc->fdc.phase == FDC_VERIFY) {
    int left = bbc->fdc.sectors_left;
    while (left > 0) {
      off = FdcSectorOff(bbc, 256);
      if (off < 0) {
        FdcFinish(bbc, FdcMissing(bbc));
        return;
      }
      bbc->fdc.sector++;
      left--;
    }
    FdcFinish(bbc, FdcOk(bbc));
    return;
  }
  if (bbc->fdc.phase == FDC_READ || bbc->fdc.phase == FDC_READ_ID) {
    if (bbc->fdc.bytes_left <= 0) {
      bbc->fdc.sectors_left--;
      if (bbc->fdc.sectors_left <= 0) {
        if (Fdc1770(bbc) && (bbc->fdc.command & 0xf0) == 0xc0) {
          bbc->fdc.sector_reg = bbc->fdc.track_reg;
        }
        FdcFinish(bbc, FdcOk(bbc));
        return;
      }
      bbc->fdc.sector++;
      if (Fdc1770(bbc)) {
        bbc->fdc.sector_reg = (uint8_t)bbc->fdc.sector;
      }
      bbc->fdc.bytes_left = bbc->fdc.sector_size;
      bbc->fdc.sector_off = -1;
    }
    if (bbc->fdc.sector_off < 0) {
      if (bbc->fdc.phase == FDC_READ_ID) {
        if (FdcNoDisc(bbc)) {
          FdcFinish(bbc, FdcMissing(bbc));
          return;
        }
        bbc->fdc.buffer[0] = (uint8_t)(Fdc1770(bbc) ? bbc->fdc.track_reg : bbc->fdc.params[0]);
        bbc->fdc.buffer[1] = (uint8_t)bbc->fdc.side;
        bbc->fdc.buffer[2] = (uint8_t)bbc->fdc.sector;
        bbc->fdc.buffer[3] = 1;
        bbc->fdc.buffer[4] = 0;
        bbc->fdc.buffer[5] = 0;
        bbc->fdc.sector_off = 0;
      } else {
        size = bbc->fdc.sector_size > 256 ? 256 : bbc->fdc.sector_size;
        off = FdcSectorOff(bbc, size > 0 ? size : 256);
        if (off < 0) {
          FdcFinish(bbc, FdcMissing(bbc));
          return;
        }
        bbc->fdc.sector_off = off;
      }
    }
    if (bbc->fdc.phase == FDC_READ_ID) {
      FdcAsk(bbc, true, bbc->fdc.buffer[bbc->fdc.sector_off]);
    } else {
      BbcDiscImage* image = FdcImage(bbc, NULL);
      if (image == NULL || bbc->fdc.sector_off < 0) {
        FdcFinish(bbc, FdcMissing(bbc));
        return;
      }
      FdcAsk(bbc, true, DiscByte(image, bbc->fdc.sector_off));
    }
    bbc->fdc.sector_off++;
    bbc->fdc.bytes_left--;
    return;
  }
  if (bbc->fdc.phase == FDC_WRITE) {
    if (bbc->fdc.sector_off < 0) {
      BbcDiscImage* image = FdcImage(bbc, NULL);
      if (image != NULL && image->protect) {
        FdcFinish(bbc, FdcProtected(bbc));
        return;
      }
      size = bbc->fdc.sector_size > 256 ? 256 : bbc->fdc.sector_size;
      off = FdcSectorOff(bbc, size > 0 ? size : 256);
      if (off < 0) {
        FdcFinish(bbc, FdcMissing(bbc));
        return;
      }
      bbc->fdc.sector_off = off;
      bbc->fdc.buf_i = 0;
    }
    bbc->fdc.phase = FDC_WRITE_WAIT;
    FdcAsk(bbc, false, 0);
  }
}

static void FdcDataTaken(BbcMachine* bbc) {
  if (bbc->fdc.phase != FDC_READ && bbc->fdc.phase != FDC_READ_ID &&
      bbc->fdc.phase != FDC_READ_TRACK) {
    return;
  }
  if (Fdc1770(bbc)) {
    if ((bbc->fdc.status & WD_DRQ) == 0) {
      return;
    }
  } else if ((bbc->fdc.status & I8_NDM) == 0) {
    return;
  }
  FdcAck(bbc);
  bbc->fdc.delay = bbc->fdc.byte_gap;
}

static void FdcDataSupplied(BbcMachine* bbc, uint8_t value) {
  int n;
  if (bbc->fdc.phase == FDC_WRITE_TRACK) {
    if (bbc->fdc.buf_i < (int)sizeof(bbc->fdc.track_image)) {
      bbc->fdc.track_image[bbc->fdc.buf_i++] = value;
    }
    bbc->fdc.data = value;
    FdcAck(bbc);
    bbc->fdc.delay = bbc->fdc.byte_gap;
    return;
  }
  if (bbc->fdc.phase != FDC_WRITE_WAIT) {
    if (bbc->fdc.phase != FDC_READ && bbc->fdc.phase != FDC_READ_ID) {
      bbc->fdc.data = value;
    }
    return;
  }
  bbc->fdc.data = value;
  if (bbc->fdc.buf_i < (int)sizeof(bbc->fdc.buffer)) {
    bbc->fdc.buffer[bbc->fdc.buf_i++] = value;
  }
  if (bbc->fdc.bytes_left > 0) {
    bbc->fdc.bytes_left--;
  }
  FdcAck(bbc);
  if (bbc->fdc.bytes_left > 0) {
    bbc->fdc.phase = FDC_WRITE;
    bbc->fdc.delay = bbc->fdc.byte_gap;
    return;
  }
  n = bbc->fdc.buf_i;
  {
    BbcDiscImage* image = FdcImage(bbc, NULL);
    if (image != NULL && bbc->fdc.sector_off >= 0 && n > 0 &&
        (size_t)bbc->fdc.sector_off < image->length) {
      size_t room = image->length - (size_t)bbc->fdc.sector_off;
      if ((size_t)n < room) {
        room = (size_t)n;
      }
      memcpy(image->data + bbc->fdc.sector_off, bbc->fdc.buffer, room);
      DiscFlushImage(image);
    }
  }
  bbc->fdc.sectors_left--;
  if (bbc->fdc.sectors_left <= 0) {
    bbc->fdc.result = FdcOk(bbc);
    bbc->fdc.phase = FDC_FINISH;
    bbc->fdc.delay = bbc->fdc.byte_gap;
    return;
  }
  bbc->fdc.sector++;
  if (Fdc1770(bbc)) {
    bbc->fdc.sector_reg = (uint8_t)bbc->fdc.sector;
  }
  bbc->fdc.bytes_left = bbc->fdc.sector_size;
  bbc->fdc.sector_off = -1;
  bbc->fdc.buf_i = 0;
  bbc->fdc.phase = FDC_WRITE;
  bbc->fdc.delay = bbc->fdc.byte_gap;
}

static void FdcResetChip(BbcMachine* bbc) {
  bbc->fdc.status = 0;
  bbc->fdc.command = 0xff;
  bbc->fdc.phase = FDC_IDLE;
  bbc->fdc.delay = 0;
  bbc->fdc.intrq = false;
  bbc->fdc.param_i = 0;
  bbc->fdc.param_n = 0;
  if (Fdc1770(bbc)) {
    bbc->fdc.sector_reg = 1;
  } else {
    bbc->fdc.track[0] = 0;
    bbc->fdc.track[1] = 0;
  }
  bbc->nmi_edge = false;
  bbc->fdc.nmi_line = false;
  UpdateNmiLine(bbc);
  FdcUpdateNmi(bbc);
}

static void I8271Start(BbcMachine* bbc) {
  int count;
  int size;
  uint8_t command = bbc->fdc.command;
  switch (command) {
    case 0x00:
    case 0x04:
      count = bbc->fdc.params[2] & 31;
      if (count == 0) {
        count = 32;
      }
      FdcBegin(bbc, FDC_VERIFY, bbc->fdc.params[1], count, 0);
      return;
    case 0x35:
      bbc->fdc.status = 0;
      bbc->fdc.phase = FDC_IDLE;
      FdcUpdateNmi(bbc);
      return;
    case 0x29:
      if (bbc->fdc.drive >= 0 && bbc->fdc.drive <= 1) {
        bbc->fdc.track[bbc->fdc.drive] = bbc->fdc.params[0];
      }
      bbc->fdc.result = 0;
      bbc->fdc.phase = FDC_FINISH;
      bbc->fdc.delay = 200;
      bbc->fdc.status = I8_BUSY;
      return;
    case 0x3a:
      bbc->fdc.status = 0;
      if (bbc->fdc.params[0] == 0x12) {
        bbc->fdc.track[0] = bbc->fdc.params[1];
      } else if (bbc->fdc.params[0] == 0x1a) {
        bbc->fdc.track[1] = bbc->fdc.params[1];
      } else if (bbc->fdc.params[0] == 0x23) {
        bbc->fdc.drvout = bbc->fdc.params[1];
      } else if (bbc->fdc.params[0] != 0x17) {
        bbc->fdc.result = 0x18;
        bbc->fdc.status = (uint8_t)(I8_RESULT | I8_INT);
      }
      FdcUpdateNmi(bbc);
      return;
    case 0x3d:
      bbc->fdc.result = 0;
      bbc->fdc.status = I8_RESULT;
      if (bbc->fdc.params[0] == 0x12) {
        bbc->fdc.result = (uint8_t)bbc->fdc.track[0];
      } else if (bbc->fdc.params[0] == 0x1a) {
        bbc->fdc.result = (uint8_t)bbc->fdc.track[1];
      } else if (bbc->fdc.params[0] == 0x23) {
        bbc->fdc.result = bbc->fdc.drvout;
      } else if (bbc->fdc.params[0] != 0x06) {
        bbc->fdc.result = 0x18;
        bbc->fdc.status = (uint8_t)(I8_RESULT | I8_INT);
      }
      FdcUpdateNmi(bbc);
      return;
    case 0x23: {
      int track = bbc->fdc.params[0];
      int side = 0;
      BbcDiscImage* image;
      uint8_t fill = bbc->fdc.params[4];
      FdcLatchSide(bbc);
      image = FdcImage(bbc, &side);
      if (image == NULL) {
        FdcFinish(bbc, 0x10);
        return;
      }
      if (image->protect) {
        FdcFinish(bbc, 0x12);
        return;
      }
      if (track >= 0 && track < image->tracks && side < image->sides) {
        int sector;
        for (sector = 0; sector < image->spt; sector++) {
          size_t off = (((size_t)track * (size_t)image->sides + (size_t)side) *
                            (size_t)image->spt +
                        (size_t)sector) *
                       256;
          if (off + 256 <= image->length) {
            memset(image->data + off, fill, 256);
          }
        }
        DiscFlushImage(image);
      }
      bbc->fdc.result = 0;
      bbc->fdc.phase = FDC_FINISH;
      bbc->fdc.delay = 400;
      bbc->fdc.status = I8_BUSY;
      return;
    }
    case 0x0a:
    case 0x0e:
      FdcBegin(bbc, FDC_WRITE, bbc->fdc.params[1], 1, 128);
      return;
    case 0x0b:
    case 0x0f:
      count = bbc->fdc.params[2] & 31;
      size = 128 << ((bbc->fdc.params[2] >> 5) & 7);
      if (count == 0) {
        count = 32;
      }
      if (size > 256) {
        size = 256;
      }
      FdcBegin(bbc, FDC_WRITE, bbc->fdc.params[1], count, size);
      return;
    case 0x12:
    case 0x16:
      FdcBegin(bbc, FDC_READ, bbc->fdc.params[1], 1, 128);
      return;
    case 0x13:
      count = bbc->fdc.params[2] & 31;
      size = 128 << ((bbc->fdc.params[2] >> 5) & 7);
      if (count == 0) {
        count = 32;
      }
      if (size > 256) {
        size = 256;
      }
      FdcBegin(bbc, FDC_READ, bbc->fdc.params[1], count, size);
      return;
    case 0x1b:
      count = bbc->fdc.params[2] & 31;
      if (count == 0) {
        count = 32;
      }
      FdcBegin(bbc, FDC_READ_ID, 0, count, 4);
      return;
    case 0x1e:
      FdcBegin(bbc, FDC_VERIFY, bbc->fdc.params[1], 1, 0);
      return;
    case 0x1f:
      count = bbc->fdc.params[2] & 31;
      if (count == 0) {
        count = 32;
      }
      FdcBegin(bbc, FDC_VERIFY, bbc->fdc.params[1], count, 0);
      return;
    default:
      FdcFinish(bbc, 0x18);
      return;
  }
}

static void I8271Command(BbcMachine* bbc, uint8_t val) {
  if (bbc->fdc.status & I8_BUSY) {
    return;
  }
  bbc->fdc.command = (uint8_t)(val & 0x3f);
  if (bbc->fdc.command == 0x17) {
    bbc->fdc.command = 0x13;
  }
  bbc->fdc.drive = (val & 0x80) ? 1 : 0;
  if (bbc->fdc.command < 0x2c) {
    bbc->fdc.drvout = (uint8_t)((bbc->fdc.drvout & (uint8_t)~0xc0) | (val & 0xc0));
  }
  bbc->fdc.param_i = 0;
  bbc->fdc.status = I8_BUSY;
  FdcUpdateNmi(bbc);
  switch (bbc->fdc.command) {
    case 0x2c: {
      unsigned result = 0x80;
      int drive = bbc->fdc.drive;
      if (bbc->fdc.drvout & 0x80) {
        result |= 0x40;
      }
      if (bbc->fdc.drvout & 0x40) {
        result |= 0x04;
      }
      if (DriveHasDisc(bbc, drive) && (bbc->fdc.spin % 400000) < 8000) {
        result |= 0x10;
      }
      {
        BbcDiscImage* image = FdcImage(bbc, NULL);
        if (image != NULL && image->protect) {
          result |= 0x08;
        }
      }
      if (drive >= 0 && drive <= 1 && bbc->fdc.track[drive] == 0) {
        result |= 0x02;
      }
      bbc->fdc.result = (uint8_t)result;
      bbc->fdc.status = I8_RESULT;
      bbc->fdc.param_n = 0;
      bbc->fdc.phase = FDC_IDLE;
      FdcUpdateNmi(bbc);
      return;
    }
    case 0x29:
    case 0x3d:
      bbc->fdc.param_n = 1;
      break;
    case 0x0a:
    case 0x0e:
    case 0x12:
    case 0x16:
    case 0x1e:
    case 0x3a:
      bbc->fdc.param_n = 2;
      break;
    case 0x0b:
    case 0x0f:
    case 0x13:
    case 0x1f:
    case 0x1b:
      bbc->fdc.param_n = 3;
      break;
    case 0x00:
    case 0x04:
    case 0x23:
      bbc->fdc.param_n = 5;
      break;
    case 0x35:
      bbc->fdc.param_n = 4;
      break;
    default:
      bbc->fdc.param_n = 0;
      FdcFinish(bbc, 0x18);
      return;
  }
}

static void I8271Param(BbcMachine* bbc, uint8_t val) {
  if (bbc->fdc.param_n <= 0 || bbc->fdc.param_i >= bbc->fdc.param_n ||
      bbc->fdc.param_i >= (int)sizeof(bbc->fdc.params)) {
    return;
  }
  bbc->fdc.params[bbc->fdc.param_i++] = val;
  if (bbc->fdc.param_i == bbc->fdc.param_n) {
    I8271Start(bbc);
  }
}

static int WdSectorCount(const BbcMachine* bbc, bool multi) {
  int spt;
  int count;
  if (!multi) {
    return 1;
  }
  spt = 10;
  {
    BbcDiscImage* image = FdcImage(bbc, NULL);
    if (image != NULL) {
      spt = image->spt;
    }
  }
  count = spt - bbc->fdc.sector_reg;
  return count < 1 ? 1 : count;
}

static void FdcCommitTrack(BbcMachine* bbc) {
  const uint8_t* bytes = bbc->fdc.track_image;
  int n = bbc->fdc.buf_i;
  int i = 0;
  int saved_track = bbc->fdc.track_reg;
  int saved_sector = bbc->fdc.sector;
  int saved_side = bbc->fdc.side;
  bool wrote = false;
  if (FdcNoDisc(bbc)) {
    bbc->fdc.result = FdcMissing(bbc);
    return;
  }
  {
    BbcDiscImage* image = FdcImage(bbc, NULL);
    if (image == NULL) {
      bbc->fdc.result = FdcMissing(bbc);
      return;
    }
    if (image->protect) {
      bbc->fdc.result = FdcProtected(bbc);
      return;
    }
  }
  while (i + 6 < n) {
    int j;
    int size;
    int off;
    if (bytes[i] != 0xfe || i < 1 || (bytes[i - 1] != 0xf5 && bytes[i - 1] != 0xa1)) {
      i++;
      continue;
    }
    size = 128 << (bytes[i + 4] & 3);
    if (size > 256) {
      size = 256;
    }
    j = i + 5;
    while (j < n && bytes[j] != 0xfb && bytes[j] != 0xf8 && bytes[j] != 0xfe) {
      j++;
    }
    if (j < n && (bytes[j] == 0xfb || bytes[j] == 0xf8) && j + 1 + size <= n) {
      bbc->fdc.track_reg = bytes[i + 1];
      bbc->fdc.sector = bytes[i + 3];
      if (bytes[i + 2] <= 1) {
        bbc->fdc.side = bytes[i + 2];
      }
      off = FdcSectorOff(bbc, size);
      if (off >= 0) {
        BbcDiscImage* image = FdcImage(bbc, NULL);
        if (image != NULL && (size_t)off < image->length) {
          size_t room = image->length - (size_t)off;
          if ((size_t)size < room) {
            room = (size_t)size;
          }
          memcpy(image->data + off, bytes + j + 1, room);
          wrote = true;
        }
      }
      i = j + 1 + size;
      continue;
    }
    i++;
  }
  bbc->fdc.track_reg = (uint8_t)saved_track;
  bbc->fdc.sector = saved_sector;
  bbc->fdc.side = saved_side;
  if (wrote) {
    DiscFlushImage(FdcImage(bbc, NULL));
  }
  bbc->fdc.result = FdcOk(bbc);
}

static int FdcBuildTrack(BbcMachine* bbc) {
  uint8_t* out = bbc->fdc.track_image;
  int n = 0;
  int sector;
  int spt = 16;
  int saved = bbc->fdc.sector;
  int gap;
  BbcDiscImage* image;
  if (FdcNoDisc(bbc)) {
    return 0;
  }
  image = FdcImage(bbc, NULL);
  spt = image->spt;
  for (sector = 0; sector < spt; sector++) {
    int off;
    int k;
    if (n + 320 >= (int)sizeof(bbc->fdc.track_image)) {
      break;
    }
    for (gap = 0; gap < 12; gap++) {
      out[n++] = 0x00;
    }
    out[n++] = 0xf5;
    out[n++] = 0xf5;
    out[n++] = 0xf5;
    out[n++] = 0xfe;
    out[n++] = bbc->fdc.track_reg;
    out[n++] = (uint8_t)bbc->fdc.side;
    out[n++] = (uint8_t)sector;
    out[n++] = 0x01;
    out[n++] = 0xf7;
    for (gap = 0; gap < 22; gap++) {
      out[n++] = 0x4e;
    }
    for (gap = 0; gap < 12; gap++) {
      out[n++] = 0x00;
    }
    out[n++] = 0xf5;
    out[n++] = 0xf5;
    out[n++] = 0xf5;
    out[n++] = 0xfb;
    bbc->fdc.sector = sector;
    off = FdcSectorOff(bbc, 256);
    for (k = 0; k < 256; k++) {
      out[n++] = off >= 0 ? DiscByte(image, off + k) : 0xe5;
    }
    out[n++] = 0xf7;
  }
  bbc->fdc.sector = saved;
  return n;
}

static bool FdcEnabled(const BbcMachine* bbc) {
  // The Model B holds the 1770 in reset while bit 5 is clear. The Master
  // uses bit 2 for that, and bit 5 selects single density.
  if (bbc->master) {
    return (bbc->fdc.control & 0x04) != 0;
  }
  return (bbc->fdc.control & 0x20) != 0;
}

static void WdCommand(BbcMachine* bbc, uint8_t val) {
  uint8_t command;
  int track;
  if (!FdcEnabled(bbc)) {
    return;
  }
  command = (uint8_t)(val & 0xf0);
  if ((bbc->fdc.status & WD_BUSY) && command != 0xd0) {
    return;
  }
  bbc->fdc.command = command;
  bbc->fdc.intrq = false;
  if (command == 0xd0) {
    if (bbc->fdc.phase == FDC_WRITE_TRACK) {
      FdcCommitTrack(bbc);
    }
    bbc->fdc.phase = FDC_IDLE;
    bbc->fdc.delay = 0;
    bbc->fdc.status = WD_MOTOR;
    bbc->fdc.intrq = (val & 0x08) != 0;
    FdcUpdateNmi(bbc);
    return;
  }
  bbc->fdc.status = (uint8_t)(WD_MOTOR | WD_BUSY);
  FdcUpdateNmi(bbc);
  track = bbc->fdc.drive >= 0 && bbc->fdc.drive <= 1 ? bbc->fdc.track[bbc->fdc.drive] : 0;
  switch (command) {
    case 0x00:
      bbc->fdc.track_reg = 0;
      if (bbc->fdc.drive >= 0 && bbc->fdc.drive <= 1) {
        bbc->fdc.track[bbc->fdc.drive] = 0;
      }
      bbc->fdc.result = (uint8_t)(WD_MOTOR | WD_TRACK0);
      bbc->fdc.phase = FDC_FINISH;
      bbc->fdc.delay = 200;
      return;
    case 0x10:
      bbc->fdc.track_reg = bbc->fdc.data;
      if (bbc->fdc.drive >= 0 && bbc->fdc.drive <= 1) {
        bbc->fdc.track[bbc->fdc.drive] = bbc->fdc.data;
      }
      bbc->fdc.result = bbc->fdc.track_reg == 0 ? (uint8_t)(WD_MOTOR | WD_TRACK0) : WD_MOTOR;
      bbc->fdc.phase = FDC_FINISH;
      bbc->fdc.delay = 200;
      return;
    case 0x20:
    case 0x30:
    case 0x40:
    case 0x50:
    case 0x60:
    case 0x70:
      if (command == 0x40 || command == 0x50) {
        bbc->fdc.direction = 1;
      } else if (command == 0x60 || command == 0x70) {
        bbc->fdc.direction = -1;
      }
      track += bbc->fdc.direction;
      if (track < 0) {
        track = 0;
      }
      if (track > 255) {
        track = 255;
      }
      if (bbc->fdc.drive >= 0 && bbc->fdc.drive <= 1) {
        bbc->fdc.track[bbc->fdc.drive] = track;
      }
      if (command & 0x10) {
        bbc->fdc.track_reg = (uint8_t)track;
      }
      bbc->fdc.result = bbc->fdc.track_reg == 0 ? (uint8_t)(WD_MOTOR | WD_TRACK0) : WD_MOTOR;
      bbc->fdc.phase = FDC_FINISH;
      bbc->fdc.delay = 200;
      return;
    case 0x80:
    case 0x90:
      FdcBegin(bbc, FDC_READ, bbc->fdc.sector_reg, WdSectorCount(bbc, command == 0x90), 256);
      return;
    case 0xa0:
    case 0xb0:
      FdcBegin(bbc, FDC_WRITE, bbc->fdc.sector_reg, WdSectorCount(bbc, command == 0xb0), 256);
      return;
    case 0xc0:
      FdcBegin(bbc, FDC_READ_ID, 0, 1, 6);
      return;
    case 0xe0:
      if (FdcNoDisc(bbc)) {
        bbc->fdc.result = FdcMissing(bbc);
        bbc->fdc.phase = FDC_FINISH;
        bbc->fdc.delay = 200;
        return;
      }
      bbc->fdc.bytes_left = FdcBuildTrack(bbc);
      bbc->fdc.buf_i = 0;
      bbc->fdc.track_left = 400000;
      bbc->fdc.byte_gap = (bbc->fdc.control & 0x08) == 0 ? 64 : 128;
      bbc->fdc.phase = FDC_READ_TRACK;
      bbc->fdc.delay = 400;
      bbc->fdc.status = (uint8_t)(WD_MOTOR | WD_BUSY);
      FdcUpdateNmi(bbc);
      return;
    case 0xf0:
      if (FdcNoDisc(bbc)) {
        bbc->fdc.result = FdcMissing(bbc);
        bbc->fdc.phase = FDC_FINISH;
        bbc->fdc.delay = 200;
        return;
      }
      {
        BbcDiscImage* image = FdcImage(bbc, NULL);
        if (image != NULL && image->protect) {
          bbc->fdc.result = FdcProtected(bbc);
          bbc->fdc.phase = FDC_FINISH;
          bbc->fdc.delay = 200;
          return;
        }
      }
      bbc->fdc.buf_i = 0;
      bbc->fdc.track_left = 400000;
      bbc->fdc.byte_gap = (bbc->fdc.control & 0x08) == 0 ? 64 : 128;
      bbc->fdc.phase = FDC_WRITE_TRACK;
      bbc->fdc.delay = 400;
      bbc->fdc.status = (uint8_t)(WD_MOTOR | WD_BUSY);
      FdcUpdateNmi(bbc);
      return;
    default:
      bbc->fdc.result = (uint8_t)(WD_MOTOR | WD_RNF);
      bbc->fdc.phase = FDC_FINISH;
      bbc->fdc.delay = 200;
      return;
  }
}

static void FdcAdvance(BbcMachine* bbc, int cycles) {
  bbc->fdc.spin += cycles;
  while (bbc->fdc.spin >= 400000) {
    bbc->fdc.spin -= 400000;
  }
  if ((bbc->fdc.phase == FDC_WRITE_TRACK || bbc->fdc.phase == FDC_READ_TRACK) &&
      bbc->fdc.track_left > 0) {
    bbc->fdc.track_left -= cycles;
    if (bbc->fdc.track_left <= 0) {
      bbc->fdc.track_left = 0;
      if (bbc->fdc.phase == FDC_WRITE_TRACK) {
        FdcCommitTrack(bbc);
      } else {
        bbc->fdc.result = FdcOk(bbc);
      }
      bbc->fdc.phase = FDC_FINISH;
      bbc->fdc.delay = 1;
    }
  }
  if (bbc->fdc.delay <= 0) {
    return;
  }
  bbc->fdc.delay -= cycles;
  if (bbc->fdc.delay > 0) {
    return;
  }
  bbc->fdc.delay = 0;
  FdcService(bbc);
}

static void WriteMasterFdcControl(BbcMachine* bbc, uint8_t value) {
  // Master latch: bit 2 low resets the 1770, bit 1 selects drive 1, bit 4
  // selects the side. Bit 5 low is MFM; the image geometry already says so.
  if ((value & 0x04) == 0) {
    FdcResetChip(bbc);
    bbc->fdc.control = value;
    return;
  }
  bbc->fdc.control = value;
  bbc->fdc.drive = (value & 0x02) ? 1 : 0;
  bbc->fdc.side = (value & 0x10) ? 1 : 0;
}

static int MasterFdcReg(uint8_t page) {
  switch (page & 0x0f) {
    case 0x4:
    case 0x5:
    case 0x6:
    case 0x7:
      return 0;
    case 0x8:
      return 4;
    case 0x9:
      return 5;
    case 0xa:
      return 6;
    case 0xb:
      return 7;
    default:
      return -1;
  }
}

// The paged ROM is the filing system the OS just called. Disc NMIs then run
// from RAM with BASIC paged in, so the controller stays with that ROM until
// a different filing system uses the drive.
static void FdcFollowRom(BbcMachine* bbc) {
  int slot;
  int kind;
  if (bbc->fdc.forced || bbc->master) {
    return;
  }
  if (bbc->fdc.phase != FDC_IDLE && bbc->fdc.phase != FDC_FINISH) {
    return;
  }
  slot = bbc->romsel & 0x0f;
  kind = bbc->rom_fdc[slot];
  if ((kind != BBC_FDC_8271 && kind != BBC_FDC_1770) || kind == bbc->fdc.kind) {
    return;
  }
  bbc->fdc.kind = kind;
  FdcResetChip(bbc);
}

static uint8_t FdcRead(BbcMachine* bbc, uint8_t reg) {
  FdcFollowRom(bbc);
  reg = (uint8_t)(reg & 7);
  if (Fdc1770(bbc)) {
    switch (reg) {
      case 0:
        // The Model B control latch is write-only. ADFS 1.30's 1770 probe
        // reads it and treats a zero in the drive-select bits as "no
        // controller", which is what a read-back of the reset latch would
        // look like. An empty SHEILA read is &FE, and that passes the probe.
        return 0xfe;
      case 4: {
        uint8_t st = bbc->fdc.status;
        uint8_t group = (uint8_t)(bbc->fdc.command & 0xf0);
        bool type_ii = bbc->fdc.command != 0xff && group >= 0x80 && group != 0xd0;
        bbc->fdc.intrq = false;
        FdcUpdateNmi(bbc);
        // Type I status uses bit 1 as the index pulse. Type II uses it as DRQ.
        if (!type_ii) {
          st = (uint8_t)(st & (uint8_t)~0x02);
          if (bbc->fdc.spin < 8000) {
            st = (uint8_t)(st | 0x02);
          }
        }
        return st;
      }
      case 5:
        return bbc->fdc.track_reg;
      case 6:
        return bbc->fdc.sector_reg;
      case 7:
        FdcDataTaken(bbc);
        return bbc->fdc.data;
      default:
        return 0xfe;
    }
  }
  switch (reg) {
    case 0:
      return bbc->fdc.status;
    case 1:
      bbc->fdc.status = (uint8_t)(bbc->fdc.status & (uint8_t)~(I8_RESULT | I8_INT));
      FdcUpdateNmi(bbc);
      return bbc->fdc.result;
    case 4:
      FdcDataTaken(bbc);
      return bbc->fdc.data;
    default:
      return 0xfe;
  }
}

static void FdcWrite(BbcMachine* bbc, uint8_t reg, uint8_t value) {
  FdcFollowRom(bbc);
  reg = (uint8_t)(reg & 7);
  if (Fdc1770(bbc)) {
    switch (reg) {
      case 0:
        bbc->fdc.control = value;
        if ((value & 0x20) == 0) {
          FdcResetChip(bbc);
          bbc->fdc.control = value;
          return;
        }
        if (value & 0x01) {
          bbc->fdc.drive = 0;
        } else if (value & 0x02) {
          bbc->fdc.drive = 1;
        } else {
          bbc->fdc.drive = -1;
        }
        bbc->fdc.side = (value & 0x04) ? 1 : 0;
        return;
      case 4:
        WdCommand(bbc, value);
        return;
      case 5:
        bbc->fdc.track_reg = value;
        return;
      case 6:
        if (FdcEnabled(bbc)) {
          bbc->fdc.sector_reg = value;
        }
        return;
      case 7:
        FdcDataSupplied(bbc, value);
        return;
      default:
        return;
    }
  }
  switch (reg) {
    case 0:
      I8271Command(bbc, value);
      return;
    case 1:
      I8271Param(bbc, value);
      return;
    case 2:
      if (value & 0x01) {
        FdcResetChip(bbc);
      }
      return;
    case 4:
      FdcDataSupplied(bbc, value);
      return;
    default:
      return;
  }
}

static void TapeFlush(BbcMachine* bbc);
static void TapeClose(BbcMachine* bbc);
static void TapeStop(BbcMachine* bbc);

// The 68B54 ADLC. Econet is this chip, not the RS423 6850 at &FE08.
// A byte on a 200 kHz network clock is 80 CPU cycles at 2 MHz.
#define ECONET_MAX_FRAME 2048
#define ECONET_GROUP "239.255.19.82"
#define CR1_AC 0x01
#define CR1_RIE 0x02
#define CR1_TIE 0x04
#define CR1_RDSR 0x08
#define CR1_TDSR 0x10
#define CR1_RDIS 0x20
#define CR1_RXRS 0x40
#define CR1_TXRS 0x80
#define CR2_TWO 0x02
#define CR2_FC 0x08
#define CR2_TXLAST 0x10
#define CR2_CLRRX 0x20
#define CR2_CLRTX 0x40
#define CR3_FDSE 0x10
#define CR4_ABT 0x20

static pthread_mutex_t g_econet_mu = PTHREAD_MUTEX_INITIALIZER;
static BbcMachine* g_econet_peers[16];
static int g_econet_peers_n = 0;
static uint32_t g_econet_next_instance = 0;

static void EconetRefreshIrq(BbcMachine* bbc);

static bool EconetTdra(const BbcMachine* bbc) {
  int room = (bbc->econet.cr2 & CR2_TWO) != 0 ? 2 : 3;
  return (bbc->econet.cr1 & CR1_TXRS) == 0 && bbc->econet.tx_count < room;
}

static bool EconetRda(const BbcMachine* bbc) {
  if ((bbc->econet.cr1 & CR1_RXRS) != 0) {
    return false;
  }
  if ((bbc->econet.cr2 & CR2_TWO) != 0) {
    return bbc->econet.rx_count >= 2;
  }
  return bbc->econet.rx_count > 0;
}

static bool EconetFrameEnd(const BbcMachine* bbc) {
  return bbc->econet.rx_count > 0 && bbc->econet.rx_last[0] != 0;
}

static uint8_t EconetSr2(BbcMachine* bbc) {
  bool fv = bbc->econet.fv_latched || (EconetFrameEnd(bbc) && !bbc->econet.rx_short);
  bool err = bbc->econet.err_latched || (EconetFrameEnd(bbc) && bbc->econet.rx_short);
  bool ap = bbc->econet.ap;
  bool idle = bbc->econet.rx_idle;
  bool abt = bbc->econet.rx_abt;
  bool ovrn = bbc->econet.ovrn;
  bool rda = EconetRda(bbc);
  bool pse = (bbc->econet.cr2 & 0x01) != 0;
  uint8_t sr2 = 0;
  if (pse && (err || fv || abt || ovrn)) {
    ap = false;
    idle = false;
    rda = false;
  } else if (pse && idle) {
    ap = false;
    rda = false;
  } else if (pse && ap) {
    rda = false;
  }
  if (ap) {
    sr2 |= 0x01;
  }
  if (fv) {
    sr2 |= 0x02;
  }
  if (idle) {
    sr2 |= 0x04;
  }
  if (abt) {
    sr2 |= 0x08;
  }
  if (err) {
    sr2 |= 0x10;
  }
  if (ovrn) {
    sr2 |= 0x40;
  }
  if (rda) {
    sr2 |= 0x80;
  }
  return sr2;
}

static uint8_t EconetSr1(BbcMachine* bbc) {
  bool tdra = EconetTdra(bbc);
  bool fc = bbc->econet.fc_latched;
  bool tx_bit = (bbc->econet.cr2 & CR2_FC) != 0 ? fc : tdra;
  bool fd = bbc->econet.fd_latched && (bbc->econet.cr3 & CR3_FDSE) != 0;
  // Idle is a level the transmit routine polls. It is not an interrupt, or a
  // quiet network would NMI on every byte.
  bool s2 = (EconetSr2(bbc) & (uint8_t)~0x84) != 0;
  bool rda = EconetRda(bbc);
  bool pse = (bbc->econet.cr2 & 0x01) != 0;
  bool txu = bbc->econet.txu;
  bool rda_service = rda && (bbc->econet.cr1 & CR1_RDSR) == 0;
  bool tx_service = tx_bit && (bbc->econet.cr1 & CR1_TDSR) == 0;
  uint8_t sr1 = 0;
  if (txu) {
    sr1 |= 0x20;
  }
  if (tx_bit) {
    sr1 |= 0x40;
  }
  if (pse && fd) {
    sr1 |= 0x08;
  } else if (pse && s2) {
    sr1 |= 0x02;
  } else if (pse && rda) {
    sr1 |= 0x01;
  } else if (!pse) {
    if (fd) {
      sr1 |= 0x08;
    }
    if (s2) {
      sr1 |= 0x02;
    }
    if (rda) {
      sr1 |= 0x01;
    }
  }
  if ((bbc->econet.cr1 & CR1_RDSR) != 0) {
    sr1 = (uint8_t)(sr1 & (uint8_t)~0x01);
  }
  // CTS is the network clock. NFS will not transmit until this is set and
  // the receiver reports the line idle. A fitted interface has the clock on.
  sr1 |= 0x10;
  if (((bbc->econet.cr1 & CR1_RIE) != 0 && (rda_service || fd || s2 || bbc->econet.ap)) ||
      ((bbc->econet.cr1 & CR1_TIE) != 0 && (tx_service || txu))) {
    sr1 |= 0x80;
  }
  return sr1;
}

static bool EconetIrqActive(BbcMachine* bbc) {
  return (EconetSr1(bbc) & 0x80) != 0;
}

static void EconetRefreshIrq(BbcMachine* bbc) {
  if (!bbc->econet.fitted) {
    bbc->econet.irq = false;
    UpdateNmiLine(bbc);
    return;
  }
  bbc->econet.irq = EconetIrqActive(bbc);
  UpdateNmiLine(bbc);
}

static void EconetResetRx(BbcMachine* bbc) {
  bbc->econet.rx_count = 0;
  bbc->econet.rx_active = false;
  bbc->econet.rx_pos = 0;
  bbc->econet.rx_len = 0;
  bbc->econet.ap = false;
  bbc->econet.fv_latched = false;
  bbc->econet.err_latched = false;
  bbc->econet.ovrn = false;
  bbc->econet.rx_abt = false;
  bbc->econet.rx_idle = false;
  bbc->econet.fd_latched = false;
  bbc->econet.rx_short = false;
}

static void EconetResetTx(BbcMachine* bbc) {
  bbc->econet.tx_count = 0;
  bbc->econet.tx_len = 0;
  bbc->econet.tx_in_frame = false;
  bbc->econet.txu = false;
  bbc->econet.fc_latched = false;
}

static bool EconetLocalInstance(uint32_t instance) {
  int i;
  for (i = 0; i < g_econet_peers_n; i++) {
    if (g_econet_peers[i]->econet.instance == instance) {
      return true;
    }
  }
  return false;
}

static void EconetInbox(BbcMachine* bbc, const uint8_t* data, int len) {
  if (len <= 0 || len > ECONET_MAX_FRAME) {
    return;
  }
  if (bbc->econet.tx_in_frame) {
    bbc->econet.txu = true;
    bbc->econet.rx_abt = true;
    bbc->econet.tx_in_frame = false;
    bbc->econet.tx_len = 0;
    bbc->econet.tx_count = 0;
    return;
  }
  if (bbc->econet.inbox_n >= 4) {
    return;
  }
  memcpy(bbc->econet.inbox[bbc->econet.inbox_n], data, (size_t)len);
  bbc->econet.inbox_len[bbc->econet.inbox_n] = len;
  bbc->econet.inbox_n++;
}

static void EconetPublish(BbcMachine* bbc, const uint8_t* data, int len) {
  uint8_t packet[14 + ECONET_MAX_FRAME];
  struct sockaddr_in dest;
  int i;
  if (len <= 0 || len > ECONET_MAX_FRAME) {
    return;
  }
  pthread_mutex_lock(&g_econet_mu);
  for (i = 0; i < g_econet_peers_n; i++) {
    BbcMachine* peer = g_econet_peers[i];
    if (peer != bbc && peer->econet.port == bbc->econet.port) {
      EconetInbox(peer, data, len);
    }
  }
  pthread_mutex_unlock(&g_econet_mu);
  if (bbc->econet.fd < 0) {
    return;
  }
  memcpy(packet, "ECONET01", 8);
  packet[8] = (uint8_t)(bbc->econet.instance >> 24);
  packet[9] = (uint8_t)(bbc->econet.instance >> 16);
  packet[10] = (uint8_t)(bbc->econet.instance >> 8);
  packet[11] = (uint8_t)bbc->econet.instance;
  packet[12] = (uint8_t)(len >> 8);
  packet[13] = (uint8_t)len;
  memcpy(packet + 14, data, (size_t)len);
  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_port = htons((uint16_t)bbc->econet.port);
  inet_pton(AF_INET, ECONET_GROUP, &dest.sin_addr);
  sendto(bbc->econet.fd, (const char*)packet, 14 + len, 0, (struct sockaddr*)&dest, sizeof(dest));
}

static void EconetPoll(BbcMachine* bbc) {
  if (bbc->econet.fd < 0) {
    return;
  }
  for (;;) {
    uint8_t buf[14 + ECONET_MAX_FRAME];
    ssize_t n = recvfrom(bbc->econet.fd, (char*)buf, (int)sizeof(buf), 0, NULL, NULL);
    uint32_t instance;
    int len;
    if (n < 0) {
      return;
    }
    if (n < 14 || memcmp(buf, "ECONET01", 8) != 0) {
      continue;
    }
    instance = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) | ((uint32_t)buf[10] << 8) |
               (uint32_t)buf[11];
    len = ((int)buf[12] << 8) | buf[13];
    if (len <= 0 || len > ECONET_MAX_FRAME || 14 + len > (int)n) {
      continue;
    }
    pthread_mutex_lock(&g_econet_mu);
    if (instance != bbc->econet.instance && !EconetLocalInstance(instance)) {
      EconetInbox(bbc, buf + 14, len);
    }
    pthread_mutex_unlock(&g_econet_mu);
  }
}

static void EconetTxTick(BbcMachine* bbc) {
  uint8_t byte;
  bool last;
  int i;
  if ((bbc->econet.cr1 & CR1_TXRS) != 0) {
    return;
  }
  if (!bbc->econet.tx_in_frame) {
    if (bbc->econet.tx_count == 0) {
      return;
    }
    bbc->econet.tx_in_frame = true;
    bbc->econet.tx_len = 0;
  }
  if (bbc->econet.tx_count == 0) {
    bbc->econet.txu = true;
    bbc->econet.tx_in_frame = false;
    bbc->econet.tx_len = 0;
    return;
  }
  byte = bbc->econet.tx_fifo[0];
  last = bbc->econet.tx_last[0] != 0;
  for (i = 1; i < bbc->econet.tx_count; i++) {
    bbc->econet.tx_fifo[i - 1] = bbc->econet.tx_fifo[i];
    bbc->econet.tx_last[i - 1] = bbc->econet.tx_last[i];
  }
  bbc->econet.tx_count--;
  if (bbc->econet.tx_len < ECONET_MAX_FRAME) {
    bbc->econet.tx_frame[bbc->econet.tx_len++] = byte;
  }
  if (last || bbc->econet.tx_len == ECONET_MAX_FRAME) {
    EconetPublish(bbc, bbc->econet.tx_frame, bbc->econet.tx_len);
    bbc->econet.fc_latched = true;
    bbc->econet.tx_in_frame = false;
    bbc->econet.tx_len = 0;
  }
}

static bool EconetTakeInbox(BbcMachine* bbc) {
  int i;
  int len;
  bool took = false;
  pthread_mutex_lock(&g_econet_mu);
  if (bbc->econet.inbox_n > 0) {
    len = bbc->econet.inbox_len[0];
    memcpy(bbc->econet.rx_frame, bbc->econet.inbox[0], (size_t)len);
    bbc->econet.rx_len = len;
    bbc->econet.rx_short = len < 4;
    for (i = 1; i < bbc->econet.inbox_n; i++) {
      memcpy(bbc->econet.inbox[i - 1], bbc->econet.inbox[i], (size_t)bbc->econet.inbox_len[i]);
      bbc->econet.inbox_len[i - 1] = bbc->econet.inbox_len[i];
    }
    bbc->econet.inbox_n--;
    took = true;
  }
  pthread_mutex_unlock(&g_econet_mu);
  return took;
}

static void EconetRxTick(BbcMachine* bbc) {
  int pos;
  if ((bbc->econet.cr1 & CR1_RXRS) != 0) {
    return;
  }
  if ((bbc->econet.cr1 & CR1_RDIS) != 0) {
    bbc->econet.rx_count = 0;
    bbc->econet.rx_active = false;
    bbc->econet.rx_pos = 0;
    bbc->econet.cr1 = (uint8_t)(bbc->econet.cr1 & (uint8_t)~CR1_RDIS);
    return;
  }
  if (bbc->econet.fv_latched || bbc->econet.err_latched) {
    return;
  }
  if (bbc->econet.rx_count >= 3) {
    if (bbc->econet.rx_active && bbc->econet.rx_pos < bbc->econet.rx_len) {
      bbc->econet.ovrn = true;
    }
    return;
  }
  if (!bbc->econet.rx_active) {
    if (!EconetTakeInbox(bbc)) {
      // Fifteen ones on a quiet wire. NFS waits for this before a scout,
      // and reports "Line Jammed" when it never arrives.
      if (bbc->econet.rx_count == 0 && !bbc->econet.fv_latched && !bbc->econet.err_latched) {
        bbc->econet.rx_idle = true;
      }
      return;
    }
    bbc->econet.rx_idle = false;
    bbc->econet.rx_active = true;
    bbc->econet.rx_pos = 0;
  }
  pos = bbc->econet.rx_count;
  bbc->econet.rx_fifo[pos] = bbc->econet.rx_frame[bbc->econet.rx_pos];
  bbc->econet.rx_first[pos] = bbc->econet.rx_pos == 0 ? 1 : 0;
  bbc->econet.rx_last[pos] =
      bbc->econet.rx_pos + 1 == bbc->econet.rx_len ? 1 : 0;
  bbc->econet.rx_count++;
  bbc->econet.rx_pos++;
  if (bbc->econet.rx_pos >= bbc->econet.rx_len) {
    bbc->econet.rx_active = false;
  }
  if (bbc->econet.rx_first[pos] != 0 && pos == 0) {
    bbc->econet.ap = true;
  } else if (bbc->econet.rx_first[0] != 0) {
    bbc->econet.ap = true;
  }
}

static void EconetAdvance(BbcMachine* bbc, int cycles) {
  if (!bbc->econet.fitted || cycles <= 0) {
    return;
  }
  bbc->econet.poll_acc += cycles;
  if (bbc->econet.poll_acc >= 400) {
    bbc->econet.poll_acc = 0;
    EconetPoll(bbc);
  }
  bbc->econet.bit_acc += cycles;
  while (bbc->econet.bit_acc >= 80) {
    bbc->econet.bit_acc -= 80;
    if ((bbc->econet.cr1 & CR1_RXRS) == 0 && (bbc->econet.cr3 & CR3_FDSE) != 0) {
      bbc->econet.fd_latched = true;
    }
    EconetTxTick(bbc);
    EconetRxTick(bbc);
  }
  EconetRefreshIrq(bbc);
}

static void EconetWriteCr1(BbcMachine* bbc, uint8_t value) {
  bbc->econet.cr1 = value;
  if ((value & CR1_RXRS) != 0) {
    EconetResetRx(bbc);
  }
  if ((value & CR1_TXRS) != 0) {
    EconetResetTx(bbc);
  }
  EconetRefreshIrq(bbc);
}

static void EconetWriteCr2(BbcMachine* bbc, uint8_t value) {
  if ((value & CR2_CLRRX) != 0) {
    if ((bbc->econet.last_sr1 & 0x08) != 0) {
      bbc->econet.fd_latched = false;
    }
    if ((bbc->econet.last_sr2 & 0x02) != 0) {
      bbc->econet.fv_latched = false;
    }
    if ((bbc->econet.last_sr2 & 0x04) != 0) {
      bbc->econet.rx_idle = false;
    }
    if ((bbc->econet.last_sr2 & 0x08) != 0) {
      bbc->econet.rx_abt = false;
    }
    if ((bbc->econet.last_sr2 & 0x10) != 0) {
      bbc->econet.err_latched = false;
    }
    if ((bbc->econet.last_sr2 & 0x40) != 0) {
      bbc->econet.ovrn = false;
    }
  }
  if ((value & CR2_CLRTX) != 0 && (bbc->econet.last_sr1 & 0x20) != 0) {
    bbc->econet.txu = false;
  }
  if ((value & CR2_TXLAST) != 0 && bbc->econet.tx_count > 0) {
    bbc->econet.tx_last[bbc->econet.tx_count - 1] = 1;
  }
  if ((value & CR2_FC) == 0) {
    bbc->econet.fc_latched = false;
  }
  bbc->econet.cr2 = (uint8_t)(value & (uint8_t)~(CR2_TXLAST | CR2_CLRRX | CR2_CLRTX));
  EconetRefreshIrq(bbc);
}

static void EconetPushTx(BbcMachine* bbc, uint8_t value, bool last) {
  if ((bbc->econet.cr1 & CR1_TXRS) != 0 || bbc->econet.tx_count >= 3) {
    return;
  }
  bbc->econet.tx_fifo[bbc->econet.tx_count] = value;
  bbc->econet.tx_last[bbc->econet.tx_count] = last ? 1 : 0;
  bbc->econet.tx_count++;
  EconetRefreshIrq(bbc);
}

static void EconetWrite(BbcMachine* bbc, int rs, uint8_t value) {
  if (!bbc->econet.fitted) {
    return;
  }
  if (rs == 0) {
    EconetWriteCr1(bbc, value);
    return;
  }
  if (rs == 1) {
    if ((bbc->econet.cr1 & CR1_AC) != 0) {
      bbc->econet.cr3 = value;
      EconetRefreshIrq(bbc);
    } else {
      EconetWriteCr2(bbc, value);
    }
    return;
  }
  if (rs == 2 || (bbc->econet.cr1 & CR1_AC) == 0) {
    EconetPushTx(bbc, value, rs == 3);
    return;
  }
  bbc->econet.cr4 = (uint8_t)(value & (uint8_t)~CR4_ABT);
  if ((value & CR4_ABT) != 0) {
    bbc->econet.tx_count = 0;
    bbc->econet.tx_in_frame = false;
    bbc->econet.tx_len = 0;
    EconetRefreshIrq(bbc);
  }
}

static uint8_t EconetReadData(BbcMachine* bbc) {
  uint8_t byte = 0;
  bool last;
  int i;
  if (bbc->econet.rx_count <= 0) {
    return 0;
  }
  byte = bbc->econet.rx_fifo[0];
  last = bbc->econet.rx_last[0] != 0;
  if (bbc->econet.rx_first[0] != 0) {
    bbc->econet.ap = false;
  }
  for (i = 1; i < bbc->econet.rx_count; i++) {
    bbc->econet.rx_fifo[i - 1] = bbc->econet.rx_fifo[i];
    bbc->econet.rx_last[i - 1] = bbc->econet.rx_last[i];
    bbc->econet.rx_first[i - 1] = bbc->econet.rx_first[i];
  }
  bbc->econet.rx_count--;
  if (bbc->econet.rx_count > 0 && bbc->econet.rx_first[0] != 0) {
    bbc->econet.ap = true;
  }
  if (last) {
    if (bbc->econet.rx_short) {
      bbc->econet.err_latched = true;
    } else {
      bbc->econet.fv_latched = true;
    }
  }
  EconetRefreshIrq(bbc);
  return byte;
}

static uint8_t EconetRead(BbcMachine* bbc, int rs) {
  uint8_t value;
  if (!bbc->econet.fitted) {
    return 0xfe;
  }
  if (rs == 0) {
    value = EconetSr1(bbc);
    bbc->econet.last_sr1 = value;
    return value;
  }
  if (rs == 1) {
    value = EconetSr2(bbc);
    bbc->econet.last_sr2 = value;
    return value;
  }
  return EconetReadData(bbc);
}

static void EconetLeave(BbcMachine* bbc) {
  int i;
  pthread_mutex_lock(&g_econet_mu);
  for (i = 0; i < g_econet_peers_n; i++) {
    if (g_econet_peers[i] == bbc) {
      g_econet_peers[i] = g_econet_peers[g_econet_peers_n - 1];
      g_econet_peers_n--;
      break;
    }
  }
  pthread_mutex_unlock(&g_econet_mu);
}

static void EconetClose(BbcMachine* bbc) {
  if (!bbc->econet.fitted && bbc->econet.fd < 0) {
    return;
  }
  EconetLeave(bbc);
  if (bbc->econet.fd >= 0) {
    SocketClose(bbc->econet.fd);
    bbc->econet.fd = -1;
  }
  bbc->econet.fitted = false;
}

static int EconetBind(BbcMachine* bbc, int port) {
  int fd;
  int on = 1;
  struct sockaddr_in addr;
  struct ip_mreq mreq;
  if (!SocketStartup()) {
    return -1;
  }
  fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof(on));
#endif
  memset(&addr, 0, sizeof(addr));
#ifdef __APPLE__
  addr.sin_len = sizeof(addr);
#endif
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    SocketClose(fd);
    return -1;
  }
  memset(&mreq, 0, sizeof(mreq));
  inet_pton(AF_INET, ECONET_GROUP, &mreq.imr_multiaddr);
#ifdef __APPLE__
  // The Wi-Fi interface is simplex, and a looped multicast packet comes
  // back with the LAN address as its source. An ordinary program then
  // never sees it. Loopback keeps the wire on this machine.
  inet_pton(AF_INET, "127.0.0.1", &mreq.imr_interface);
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&mreq.imr_interface,
             sizeof(mreq.imr_interface));
#else
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
#endif
  if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) != 0) {
    SocketClose(fd);
    return -1;
  }
  SocketSetNonBlocking(fd);
  bbc->econet.fd = fd;
  return 0;
}

int BbcMachineOpenEconet(BbcMachine* bbc, int station, int port) {
  if (bbc == NULL || station < 1 || station > 254 || port < 1 || port > 65535) {
    return -1;
  }
  EconetClose(bbc);
  memset(&bbc->econet, 0, sizeof(bbc->econet));
  bbc->econet.fd = -1;
  bbc->econet.fitted = true;
  bbc->econet.station = station;
  bbc->econet.port = port;
  bbc->econet.cr1 = (uint8_t)(CR1_RXRS | CR1_TXRS);
  if (bbc->master) {
    bbc->cmos[14] = (uint8_t)station;
  }
  pthread_mutex_lock(&g_econet_mu);
  // Every process on the wire hears every frame, its own included, and
  // drops the ones whose instance is local. Instances must differ between
  // processes, so they start from the process id.
  if (g_econet_next_instance == 0) {
    g_econet_next_instance = HostProcessId() << 10;
  }
  bbc->econet.instance = ++g_econet_next_instance;
  if (g_econet_peers_n < 16) {
    g_econet_peers[g_econet_peers_n++] = bbc;
  }
  pthread_mutex_unlock(&g_econet_mu);
  if (EconetBind(bbc, port) != 0) {
    return -1;
  }
  return port;
}

static void WriteSheila(BbcMachine* bbc, uint8_t page, uint8_t value) {
  if (page < 0x08) {
    if ((page & 1) == 0) {
      bbc->crtc_addr = value & 0x1f;
    } else if (bbc->crtc_addr < 32) {
      bbc->crtc[bbc->crtc_addr] = value;
    }
    return;
  }
  if (page < 0x10) {
    if ((page & 1) == 0) {
      AcaiWriteControl(bbc, value);
    } else {
      AcaiWriteData(bbc, value);
    }
    return;
  }
  if (page < 0x18) {
    bool motor = MotorOn(bbc);
    bbc->serial_ula = value;
    SerialUpdate(bbc);
    if (motor && !MotorOn(bbc)) {
      TapeFlush(bbc);
    }
    return;
  }
  if (page < 0x20) {
    // A write to &FE18-&FE1F disables Econet NMIs. The station links are read-only.
    if (bbc->econet.fitted && !bbc->master) {
      bbc->econet.nmi_enable = false;
      UpdateNmiLine(bbc);
    }
    return;
  }
  if (page < 0x30) {
    // On the Model B the video ULA select (&FE20-&FE2F) enables Econet NMIs.
    if (bbc->econet.fitted && !bbc->master) {
      bbc->econet.nmi_enable = true;
      UpdateNmiLine(bbc);
    }
    // The Master's WD1770 sits at &FE24-&FE2B. &FE20-&FE23 stay the video ULA.
    if (bbc->master && page >= 0x24) {
      int reg = MasterFdcReg(page);
      if (reg == 0) {
        WriteMasterFdcControl(bbc, value);
      } else if (reg > 0) {
        FdcWrite(bbc, (uint8_t)reg, value);
      }
      return;
    }
    if (page & 1) {
      WritePalette(bbc, value);
    } else {
      WriteControl(bbc, value);
    }
    return;
  }
  if (page < 0x40) {
    // The Master decodes ACCCON at &FE34. The Model B mirrors ROMSEL there.
    if (bbc->master && page == 0x34) {
      WriteAcccon(bbc, value);
      return;
    }
    // The Master gates Econet NMIs at &FE38 (disable) and &FE3C (enable).
    if (bbc->master && bbc->econet.fitted && (page == 0x38 || page == 0x3c)) {
      bbc->econet.nmi_enable = page == 0x3c;
      UpdateNmiLine(bbc);
      return;
    }
    bbc->romsel = value;
    MapRomsel(bbc);
    return;
  }
  if (page < 0x60) {
    ViaWrite(bbc, &bbc->sys, true, page & 0x0f, value);
    return;
  }
  if (page < 0x80) {
    ViaWrite(bbc, &bbc->user, false, page & 0x0f, value);
    return;
  }
  if (page < 0xa0) {
    // &FE80 is the Model B disc socket. The Master leaves it empty.
    if (!bbc->master) {
      FdcWrite(bbc, page, value);
    }
    return;
  }
  if (page < 0xc0) {
    EconetWrite(bbc, page & 3, value);
    return;
  }
  if (page < 0xe0) {
    if ((page & 3) == 0) {
      AdcWrite(bbc, value);
    }
    return;
  }
  // &FEE0-&FEFF is the Tube. An empty socket does not echo the probe.
  if (bbc->tube != NULL) {
    TubeHostWrite(bbc->tube, page & 7, value);
  }
}

static uint8_t ReadSheila(BbcMachine* bbc, uint8_t page) {
  if (page < 0x08) {
    if (page & 1) {
      int reg = bbc->crtc_addr;
      if (reg >= 12 && reg <= 17) {
        return bbc->crtc[reg];
      }
      return 0;
    }
    return bbc->crtc_addr;
  }
  if (page < 0x10) {
    if (page & 1) {
      uint8_t data = bbc->acia_rx;
      bbc->acia_rx_full = false;
      bbc->acia_overrun = false;
      return data;
    }
    return AcaiStatus(bbc);
  }
  if (page < 0x18) {
    return bbc->serial_ula;
  }
  if (page < 0x20) {
    if (bbc->econet.fitted) {
      // NFS masks the NMI with BIT &FE18. The read is the gate, not a write.
      if (!bbc->master) {
        bbc->econet.nmi_enable = false;
        UpdateNmiLine(bbc);
      }
      return (uint8_t)bbc->econet.station;
    }
    return 0xfe;
  }
  if (page < 0x30) {
    // BIT &FE20 is how NFS lets the NMI through again after the handler.
    if (bbc->econet.fitted && !bbc->master) {
      bbc->econet.nmi_enable = true;
      UpdateNmiLine(bbc);
    }
    if (bbc->master && page >= 0x24) {
      int reg = MasterFdcReg(page);
      if (reg > 0) {
        return FdcRead(bbc, (uint8_t)reg);
      }
      return 0xfe;
    }
    return 0xfe;
  }
  if (page < 0x40) {
    if (bbc->master && page == 0x34) {
      return (uint8_t)(bbc->acccon & (uint8_t)~0x40);
    }
    return bbc->romsel;
  }
  if (page < 0x60) {
    return ViaRead(bbc, &bbc->sys, true, page & 0x0f);
  }
  if (page < 0x80) {
    return ViaRead(bbc, &bbc->user, false, page & 0x0f);
  }
  if (page < 0xa0) {
    if (bbc->master) {
      return 0xfe;
    }
    return FdcRead(bbc, page);
  }
  if (page < 0xc0) {
    return EconetRead(bbc, page & 3);
  }
  if (page < 0xe0) {
    return AdcRead(bbc, page & 3);
  }
  if (bbc->tube != NULL) {
    return TubeHostRead(bbc->tube, page & 7);
  }
  return 0xfe;
}

// Winchester status at &FC41, as ADFS 1.30 samples it. Bit 0 is request,
// bit 1 is busy, bit 5 is set while a phase is valid, bit 6 is input to
// the host, and bit 7 is command or status rather than data.
#define HD_FREE 0
#define HD_COMMAND 1
#define HD_DATA_IN 2
#define HD_DATA_OUT 3
#define HD_STATUS 4
#define HD_MESSAGE 5
#define HD_REQ 0x01
#define HD_BSY 0x02
#define HD_VALID 0x20
#define HD_IO 0x40
#define HD_CD 0x80

static void HdFlush(BbcMachine* bbc) {
  FILE* fp;
  if (bbc == NULL || !bbc->hd.dirty || bbc->hd.protect || bbc->hd.path == NULL ||
      bbc->hd.data == NULL) {
    return;
  }
  fp = fopen(bbc->hd.path, "r+b");
  if (fp == NULL) {
    return;
  }
  if (fwrite(bbc->hd.data, 1, bbc->hd.length, fp) == bbc->hd.length) {
    bbc->hd.dirty = false;
  }
  fclose(fp);
}

static void HdRelease(BbcMachine* bbc) {
  if (bbc == NULL) {
    return;
  }
  HdFlush(bbc);
  free(bbc->hd.data);
  free(bbc->hd.path);
  memset(&bbc->hd, 0, sizeof(bbc->hd));
}

static void HdBeginStatus(BbcMachine* bbc, uint8_t status) {
  bbc->hd.phase = HD_STATUS;
  bbc->hd.status_byte = status;
  if (status == 0) {
    memset(bbc->hd.sense, 0, sizeof(bbc->hd.sense));
  }
}

static void HdFail(BbcMachine* bbc, uint8_t code) {
  memset(bbc->hd.sense, 0, sizeof(bbc->hd.sense));
  bbc->hd.sense[0] = code;
  HdBeginStatus(bbc, 0x02);
}

static int HdCdbLen(uint8_t op) {
  switch (op >> 5) {
    case 0:
      return 6;
    case 1:
    case 2:
      return 10;
    case 5:
      return 12;
    default:
      return 6;
  }
}

static uint32_t HdLba6(const uint8_t* cdb) {
  return ((uint32_t)(cdb[1] & 0x1f) << 16) | ((uint32_t)cdb[2] << 8) | (uint32_t)cdb[3];
}

static uint32_t HdCount6(const uint8_t* cdb) {
  return cdb[4] == 0 ? 256u : (uint32_t)cdb[4];
}

static bool HdRange(const BbcMachine* bbc, uint32_t lba, uint32_t sectors) {
  return sectors == 0 || (lba < bbc->hd.sectors && sectors <= bbc->hd.sectors - lba);
}

static void HdStartTransfer(BbcMachine* bbc, bool writing, uint32_t lba, uint32_t sectors) {
  if (!HdRange(bbc, lba, sectors)) {
    HdFail(bbc, 0x01);
    return;
  }
  if (writing && bbc->hd.protect) {
    HdFail(bbc, 0x44);
    return;
  }
  if (sectors == 0) {
    HdBeginStatus(bbc, 0x00);
    return;
  }
  bbc->hd.image_io = true;
  bbc->hd.pos = (size_t)lba * 256u;
  bbc->hd.remaining = (size_t)sectors * 256u;
  bbc->hd.phase = writing ? HD_DATA_OUT : HD_DATA_IN;
}

static void HdRun(BbcMachine* bbc) {
  const uint8_t* cdb = bbc->hd.cdb;
  uint8_t op = cdb[0];
  uint32_t lba;
  uint32_t count;
  if (bbc->hd.cdb_n >= 10) {
    lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) |
          (uint32_t)cdb[5];
    count = ((uint32_t)cdb[7] << 8) | (uint32_t)cdb[8];
  } else {
    lba = HdLba6(cdb);
    count = HdCount6(cdb);
  }
  switch (op) {
    case 0x00:
    case 0x01:
      HdBeginStatus(bbc, 0x00);
      break;
    case 0x03:
      memcpy(bbc->hd.scratch, bbc->hd.sense, 4);
      bbc->hd.image_io = false;
      bbc->hd.scratch_i = 0;
      bbc->hd.remaining = 4;
      bbc->hd.phase = HD_DATA_IN;
      break;
    case 0x08:
    case 0x28:
      HdStartTransfer(bbc, false, lba, count);
      break;
    case 0x0a:
    case 0x2a:
      HdStartTransfer(bbc, true, lba, count);
      break;
    case 0x0b:
    case 0x2f:
      if (!HdRange(bbc, lba, op == 0x0b ? 1u : count)) {
        HdFail(bbc, 0x01);
      } else {
        HdBeginStatus(bbc, 0x00);
      }
      break;
    default:
      HdFail(bbc, 0x01);
      break;
  }
}

static void HdSelect(BbcMachine* bbc) {
  int i;
  if (bbc->hd.phase != HD_FREE || (bbc->hd.latch & 0x01) == 0) {
    return;
  }
  bbc->hd.phase = HD_COMMAND;
  bbc->hd.cdb_i = 0;
  bbc->hd.cdb_n = 6;
  for (i = 0; i < 16; i++) {
    bbc->hd.cdb[i] = 0;
  }
}

static uint8_t HdTake(BbcMachine* bbc) {
  uint8_t byte = 0;
  bool writing = bbc->hd.phase == HD_DATA_OUT;
  if (bbc->hd.image_io) {
    if (bbc->hd.pos < bbc->hd.length) {
      byte = bbc->hd.data[bbc->hd.pos];
    }
    bbc->hd.pos++;
  } else if (bbc->hd.scratch_i < 4) {
    byte = bbc->hd.scratch[bbc->hd.scratch_i++];
  }
  if (bbc->hd.remaining > 0) {
    bbc->hd.remaining--;
  }
  if (bbc->hd.remaining == 0) {
    if (writing) {
      HdFlush(bbc);
    }
    HdBeginStatus(bbc, 0x00);
  }
  return byte;
}

static void HdPut(BbcMachine* bbc, uint8_t value) {
  if (bbc->hd.image_io && bbc->hd.pos < bbc->hd.length) {
    bbc->hd.data[bbc->hd.pos] = value;
    bbc->hd.dirty = true;
  }
  bbc->hd.pos++;
  if (bbc->hd.remaining > 0) {
    bbc->hd.remaining--;
  }
  if (bbc->hd.remaining == 0) {
    HdFlush(bbc);
    HdBeginStatus(bbc, 0x00);
  }
}

static uint8_t HdStatusBits(const BbcMachine* bbc) {
  uint8_t pins = HD_BSY | HD_VALID | HD_REQ;
  if (bbc->hd.phase == HD_COMMAND || bbc->hd.phase == HD_STATUS || bbc->hd.phase == HD_MESSAGE) {
    pins |= HD_CD;
  }
  if (bbc->hd.phase == HD_DATA_IN || bbc->hd.phase == HD_STATUS || bbc->hd.phase == HD_MESSAGE) {
    pins |= HD_IO;
  }
  // With the interrupt enabled, status phase reads as &F2: request is no
  // longer showing, and bit 4 marks the pending interrupt. ADFS claims the
  // interrupt only when the status register is exactly that value.
  if (bbc->hd.irq && bbc->hd.phase == HD_STATUS) {
    pins = (uint8_t)((pins & (uint8_t)~HD_REQ) | 0x10);
  }
  return pins;
}

static bool HdIrqPending(const BbcMachine* bbc) {
  return bbc->hd.data != NULL && bbc->hd.irq && bbc->hd.phase == HD_STATUS;
}

static uint8_t HdReadReg(BbcMachine* bbc, int reg) {
  if (reg == 1) {
    return bbc->hd.phase == HD_FREE ? 0 : HdStatusBits(bbc);
  }
  if (reg != 0) {
    return 0xff;
  }
  if (bbc->hd.phase == HD_DATA_IN) {
    return HdTake(bbc);
  }
  if (bbc->hd.phase == HD_STATUS) {
    bbc->hd.phase = HD_MESSAGE;
    return bbc->hd.status_byte;
  }
  if (bbc->hd.phase == HD_MESSAGE) {
    uint8_t message = 0;
    HdFlush(bbc);
    bbc->hd.phase = HD_FREE;
    return message;
  }
  return bbc->hd.latch;
}

static void HdWriteReg(BbcMachine* bbc, int reg, uint8_t value) {
  if (reg == 2) {
    HdSelect(bbc);
    return;
  }
  if (reg == 3) {
    bbc->hd.irq = value != 0;
    return;
  }
  if (reg != 0) {
    return;
  }
  bbc->hd.latch = value;
  if (bbc->hd.phase == HD_DATA_OUT) {
    HdPut(bbc, value);
    return;
  }
  if (bbc->hd.phase != HD_COMMAND) {
    return;
  }
  if (bbc->hd.cdb_i < 16) {
    bbc->hd.cdb[bbc->hd.cdb_i++] = value;
  }
  if (bbc->hd.cdb_i == 1) {
    bbc->hd.cdb_n = HdCdbLen(value);
  }
  if (bbc->hd.cdb_i >= bbc->hd.cdb_n) {
    HdRun(bbc);
  }
}

static bool ReadWholeFile(const char* path, uint8_t** out, size_t* length);

// The OS offers the filing system to sideways ROMs from socket 15 downwards,
// and the first one to claim it is the one that starts. DFS is fitted above
// ADFS, so a hard disc would sit behind *ADFS. Move ADFS into that higher
// socket. The socket's RAM flag stays put; only the image moves.
static void PreferAdfsFilingSystem(BbcMachine* bbc) {
  int adfs = -1;
  int dfs = -1;
  int slot;
  uint8_t* image;
  bool loaded;
  uint8_t fs;
  uint8_t fdc;
  for (slot = 15; slot >= 0; slot--) {
    if (adfs < 0 && bbc->rom_fs[slot] == BBC_FS_ADFS) {
      adfs = slot;
    }
    if (dfs < 0 && bbc->rom_fs[slot] == BBC_FS_DFS) {
      dfs = slot;
    }
  }
  if (adfs < 0) {
    if (bbc->any_sideways) {
      fprintf(stderr, "Hard disc needs an ADFS ROM\n");
    }
    return;
  }
  if (dfs < 0 || adfs > dfs) {
    return;
  }
  if (bbc->mapped_slot == adfs || bbc->mapped_slot == dfs) {
    CommitSideways(bbc);
    bbc->mapped_slot = -1;
    bbc->mapped_virt = -1;
  }
  image = bbc->sideways[adfs];
  loaded = bbc->sideways_loaded[adfs];
  fs = bbc->rom_fs[adfs];
  fdc = bbc->rom_fdc[adfs];
  bbc->sideways[adfs] = bbc->sideways[dfs];
  bbc->sideways_loaded[adfs] = bbc->sideways_loaded[dfs];
  bbc->rom_fs[adfs] = bbc->rom_fs[dfs];
  bbc->rom_fdc[adfs] = bbc->rom_fdc[dfs];
  bbc->sideways[dfs] = image;
  bbc->sideways_loaded[dfs] = loaded;
  bbc->rom_fs[dfs] = fs;
  bbc->rom_fdc[dfs] = fdc;
  MapRomsel(bbc);
  fprintf(stderr, "ADFS is the filing system (socket %d)\n", dfs);
}

bool BbcMachineLoadHardDisc(BbcMachine* bbc, const char* path) {
  char expanded[PATH_MAX];
  const char* stored_path;
  uint8_t* buf = NULL;
  size_t length = 0;
  size_t sectors;
  char* stored;
  if (bbc == NULL || path == NULL) {
    return false;
  }
  stored_path = path;
  if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
    const char* home = HostHomeDirectory();
    int written;
    if (home != NULL && home[0] != '\0') {
      written = snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
      if (written > 0 && (size_t)written < sizeof(expanded)) {
        stored_path = expanded;
      }
    }
  }
  if (!ReadWholeFile(stored_path, &buf, &length)) {
    fprintf(stderr, "Unable to read hard disc image '%s'\n", path);
    return false;
  }
  if ((length % 256u) != 0) {
    free(buf);
    fprintf(stderr, "Hard disc image '%s' is not a whole number of sectors\n", path);
    return false;
  }
  sectors = length / 256u;
  if (sectors == 0 || sectors > 0xffffffffu) {
    free(buf);
    fprintf(stderr, "Hard disc image '%s' has no sectors\n", path);
    return false;
  }
  stored = strdup(stored_path);
  if (stored == NULL) {
    free(buf);
    return false;
  }
  HdRelease(bbc);
  bbc->hd.data = buf;
  bbc->hd.length = length;
  bbc->hd.sectors = (uint32_t)sectors;
  bbc->hd.protect = access(stored, W_OK) != 0;
  bbc->hd.path = stored;
  fprintf(stderr, "Hard disc: %s (%lu sectors)\n", stored, (unsigned long)sectors);
  PreferAdfsFilingSystem(bbc);
  return true;
}

void BbcMachineWrite(BbcMachine* bbc, uint16_t addr, uint8_t value) {
  // ACCCON bit IFJ sends &FC00-&FDFF to the cartridge port. Nothing is fitted.
  if (bbc->master && (bbc->acccon & 0x20) != 0 && addr >= 0xfc00 && addr <= 0xfdff) {
    return;
  }
  if (addr >= 0xfc00 && addr <= 0xfcff) {
    // &FC40 is the Winchester adapter when a hard disc image is loaded.
    // Everywhere else, including that page with no image, a write does not
    // stick, so the ADFS probe does not see a board that is not there.
    if (bbc->hd.data != NULL && addr >= 0xfc40 && addr <= 0xfc43) {
      HdWriteReg(bbc, (int)(addr & 3), value);
      return;
    }
    if (addr == 0xfcff) {
      bbc->jim_page = value;
      bbc->fred[0xff] = value;
    }
    return;
  }
  if (addr >= 0xfd00 && addr <= 0xfdff) {
    bbc->jim[((size_t)bbc->jim_page << 8) | (addr & 0xff)] = value;
    return;
  }
  if (addr >= 0xfe00 && addr <= 0xfeff) {
    WriteSheila(bbc, (uint8_t)(addr & 0xff), value);
  }
}

uint8_t BbcMachineRead(BbcMachine* bbc, uint16_t addr) {
  if (bbc->master && (bbc->acccon & 0x40) != 0 && bbc->os_rom != NULL && addr >= 0xfc00 &&
      addr <= 0xfeff) {
    return bbc->os_rom[addr - 0xc000];
  }
  if (bbc->master && (bbc->acccon & 0x20) != 0 && addr >= 0xfc00 && addr <= 0xfdff) {
    return 0xff;
  }
  if (addr >= 0xfc00 && addr <= 0xfcff) {
    if (bbc->hd.data != NULL && addr >= 0xfc40 && addr <= 0xfc43) {
      return HdReadReg(bbc, (int)(addr & 3));
    }
    if ((addr & 0xff) == 0xff) {
      return bbc->fred[0xff];
    }
    return 0xff;
  }
  if (addr >= 0xfd00 && addr <= 0xfdff) {
    return bbc->jim[((size_t)bbc->jim_page << 8) | (addr & 0xff)];
  }
  if (addr >= 0xfe00 && addr <= 0xfeff) {
    return ReadSheila(bbc, (uint8_t)(addr & 0xff));
  }
  if (bbc->ram != NULL) {
    return bbc->ram[addr];
  }
  return 0xff;
}

static void PutBeam(BbcMachine* bbc, int x, int y, const uint8_t rgb[3]) {
  uint8_t* p;
  if (x < 0 || y < 0 || x >= BBC_FB_WIDTH || y >= BBC_FB_HEIGHT || bbc->beam_fb == NULL) {
    return;
  }
  p = bbc->beam_fb + ((size_t)y * BBC_FB_WIDTH + (size_t)x) * 3;
  p[0] = rgb[0];
  p[1] = rgb[1];
  p[2] = rgb[2];
}

static void DrawBeamBitmap(BbcMachine* bbc, int x, int y, uint16_t ma, int ra) {
  static const uint8_t kBlack[3] = {0, 0, 0};
  int ppc = PixelsPerChar(bbc);
  int ula = UlaMode(bbc);
  int pix;
  bool cursor = CursorBlinkOn(bbc) &&
                ma == (uint16_t)(((bbc->crtc[14] << 8) | bbc->crtc[15]) & 0x3fff) &&
                ra >= (bbc->crtc[10] & 0x1f) && ra <= (bbc->crtc[11] & 0x1f);
  if (!bbc->scan_disp) {
    for (pix = 0; pix < ppc; pix++) {
      PutBeam(bbc, x + pix, y, kBlack);
    }
    return;
  }
  {
    uint8_t byte = VideoByte(bbc, ma, (uint8_t)(ra & 7));
    for (pix = 0; pix < ppc; pix++) {
      uint8_t rgb[3];
      PaletteRgb(bbc, PixelIndex(ula, byte, pix), rgb);
      if (cursor) {
        rgb[0] = (uint8_t)~rgb[0];
        rgb[1] = (uint8_t)~rgb[1];
        rgb[2] = (uint8_t)~rgb[2];
      }
      PutBeam(bbc, x + pix, y, rgb);
    }
  }
}

static void DrawBeamTeletext(BbcMachine* bbc, int x, int y, uint8_t byte, int ra) {
  int ppc = PixelsPerChar(bbc);
  int max_ra = bbc->crtc[9] & 0x1f;
  int span = max_ra + 1;
  bool control = byte < 0x20;
  uint8_t glyph = byte;
  bool graphic = bbc->tt_graphics && !control && (glyph & 0x20) != 0;
  bool hidden = bbc->tt_conceal || (bbc->tt_flash && !FlashOn(bbc));
  int pix;
  if (control && bbc->tt_hold && bbc->tt_graphics && bbc->tt_held_valid) {
    glyph = bbc->tt_held;
    graphic = true;
    control = false;
  }
  if (span < 1) {
    span = 1;
  }
  {
    int half;
    int row = TeletextLineIndex(ra, span);
    for (half = 0; half < 2; half++) {
      int yy = y + half;
      if (control || hidden || glyph < 0x20) {
        for (pix = 0; pix < ppc; pix++) {
          PutBeam(bbc, x + pix, yy, kRgb[bbc->tt_bg & 7]);
        }
      } else if (graphic) {
        static const uint8_t kBit[6] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x40};
        int vspan = bbc->tt_double ? span * 2 : span;
        int virtual_ra = bbc->tt_double && bbc->tt_bottom ? ra + span : ra;
        int band = (virtual_ra * 3) / vspan;
        int s;
        if (band > 2) {
          band = 2;
        }
        for (s = 0; s < 2; s++) {
          int which = band * 2 + s;
          const uint8_t* colour = (glyph & kBit[which]) ? kRgb[bbc->tt_fg & 7] : kRgb[bbc->tt_bg & 7];
          int x0 = x + s * (ppc / 2);
          int w = ppc / 2;
          int i;
          if (bbc->tt_separated) {
            for (i = 0; i < w; i++) {
              PutBeam(bbc, x0 + i, yy, kRgb[bbc->tt_bg & 7]);
            }
            for (i = 1; i < w - 1; i++) {
              PutBeam(bbc, x0 + i, yy, colour);
            }
          } else {
            for (i = 0; i < w; i++) {
              PutBeam(bbc, x0 + i, yy, colour);
            }
          }
        }
      } else {
        uint16_t pattern =
            TeletextPattern(glyph, row, half, bbc->tt_double, bbc->tt_bottom, bbc->keyboard);
        for (pix = 0; pix < ppc; pix++) {
          const uint8_t* colour =
              TeletextPixel(pattern, pix, ppc) ? kRgb[bbc->tt_fg & 7] : kRgb[bbc->tt_bg & 7];
          PutBeam(bbc, x + pix, yy, colour);
        }
      }
    }
  }
  if (byte < 0x20) {
    TeletextApply(bbc, byte);
  } else if (bbc->tt_graphics && (byte & 0x20) != 0) {
    bbc->tt_held = byte;
    bbc->tt_held_valid = true;
  }
}

// R8 bits 4-5. 3 blanks the picture. Teletext adds the SAA5050's two-character
// delay on top of that, which is why MODE 7's first column is still column 0
// of the screen memory.
static int DisplaySkew(const BbcMachine* bbc) {
  int skew = (bbc->crtc[8] >> 4) & 3;
  if (skew >= 3) {
    return -1;
  }
  if (Teletext(bbc)) {
    return skew + 2;
  }
  return skew;
}

// CA1 follows the vsync pulse. PCR bit 0 picks the edge that latches IFR,
// and the flag stays set until the guest reads the port or clears it.
static void VideoSetVsyncPin(BbcMachine* bbc, bool level) {
  bool positive = (bbc->sys.pcr & 0x01) != 0;
  if (level == bbc->vsync_pin) {
    return;
  }
  if (positive == level) {
    bbc->sys.ifr |= 0x02;
  }
  bbc->vsync_pin = level;
}

static void VideoFlyback(BbcMachine* bbc) {
  uint8_t* shown;
  int height;
  int width;
  if (bbc->beam_size_hold) {
    // This field started when the fast clock stopped, so it is only the
    // bottom of the picture. Keep the frame already on screen.
    bbc->beam_size_hold = false;
    memset(bbc->beam_fb, 0, (size_t)BBC_FB_WIDTH * BBC_FB_HEIGHT * 3);
    bbc->beam_y = 0;
    bbc->beam_width = 0;
    bbc->line_drawn = false;
    bbc->beam_frames++;
    bbc->frames++;
    return;
  }
  shown = bbc->fb;
  height = bbc->beam_y;
  if (height < 1) {
    height = 1;
  }
  if (height > BBC_FB_HEIGHT) {
    height = BBC_FB_HEIGHT;
  }
  width = bbc->beam_width;
  if (width < 1) {
    width = 1;
  }
  if (width > BBC_FB_WIDTH) {
    width = BBC_FB_WIDTH;
  }
  bbc->fb = bbc->beam_fb;
  bbc->beam_fb = shown;
  memset(bbc->beam_fb, 0, (size_t)BBC_FB_WIDTH * BBC_FB_HEIGHT * 3);
  bbc->frame_width = width;
  bbc->frame_height = height;
  bbc->beam_y = 0;
  bbc->beam_width = 0;
  bbc->line_drawn = false;
  bbc->beam_frames++;
  bbc->frames++;
}

static void VideoEndCharacterRow(BbcMachine* bbc) {
  bbc->v_count = (bbc->v_count + 1) & 0x7f;
  bbc->scanline = 0;
  bbc->scan_disp = true;
  bbc->had_vsync_row = false;
  if (bbc->tt_row_double) {
    bbc->tt_bottom = !bbc->tt_bottom;
  } else {
    bbc->tt_bottom = false;
  }
  bbc->tt_row_double = false;
  TeletextResetLine(bbc);
}

static void VideoEndFrame(BbcMachine* bbc) {
  bbc->v_count = 0;
  bbc->next_line_start = (uint16_t)(((bbc->crtc[12] << 8) | bbc->crtc[13]) & 0x3fff);
  bbc->line_start = bbc->next_line_start;
  bbc->v_disp = true;
  bbc->scan_disp = true;
  bbc->first_scanline = true;
  bbc->do_even_frame = (bbc->video_frames & 1) == 0;
  bbc->tt_bottom = false;
  bbc->tt_row_double = false;
  TeletextResetLine(bbc);
}

static void MaybeInvertCursor(BbcMachine* bbc, int x, int y, int w, uint16_t ma, int ra) {
  int i;
  uint8_t* p;
  if (!CursorBlinkOn(bbc) || bbc->beam_fb == NULL) {
    return;
  }
  if (ma != (uint16_t)(((bbc->crtc[14] << 8) | bbc->crtc[15]) & 0x3fff)) {
    return;
  }
  if (ra < (bbc->crtc[10] & 0x1f) || ra > (bbc->crtc[11] & 0x1f)) {
    return;
  }
  for (i = 0; i < w; i++) {
    int px = x + i;
    if (px < 0 || y < 0 || px >= BBC_FB_WIDTH || y >= BBC_FB_HEIGHT) {
      continue;
    }
    p = bbc->beam_fb + ((size_t)y * BBC_FB_WIDTH + (size_t)px) * 3;
    p[0] = (uint8_t)~p[0];
    p[1] = (uint8_t)~p[1];
    p[2] = (uint8_t)~p[2];
  }
}

static void VideoEndScanline(BbcMachine* bbc) {
  bool interlace = (bbc->crtc[8] & 3) == 3;
  bool r9_hit = bbc->scanline == (bbc->crtc[9] & 0x1f);
  bool end_of_frame = false;
  bbc->first_scanline = false;
  if (bbc->end_of_frame_latched) {
    end_of_frame = true;
  }
  bbc->vpulse = (bbc->vpulse + 1) & 0x0f;
  if (bbc->line_drawn && bbc->beam_y < BBC_FB_HEIGHT) {
    bbc->beam_y += Teletext(bbc) ? 2 : 1;
    if (bbc->beam_y > BBC_FB_HEIGHT) {
      bbc->beam_y = BBC_FB_HEIGHT;
    }
  }
  if (bbc->disp_x > bbc->beam_width) {
    bbc->beam_width = bbc->disp_x;
  }
  bbc->line_drawn = false;
  bbc->disp_x = 0;
  TeletextResetLine(bbc);
  if (r9_hit) {
    bbc->line_start = bbc->next_line_start;
  }
  if (interlace) {
    bbc->scanline = (bbc->scanline + 2) & 0x1e;
  } else {
    bbc->scanline = (bbc->scanline + 1) & 0x1f;
  }
  if (!Teletext(bbc)) {
    if (((bbc->scanline >> 3) & 1) != 0) {
      bbc->scan_disp = false;
    } else {
      bbc->scan_disp = true;
    }
  }
  if (!bbc->in_vert_adjust && r9_hit) {
    VideoEndCharacterRow(bbc);
  }
  if (bbc->end_of_main && !bbc->end_of_vert_adjust) {
    bbc->in_vert_adjust = true;
  }
  if (bbc->end_of_vert_adjust) {
    bbc->in_vert_adjust = false;
    // Interlace inserts one dummy scanline on odd frames so the field is
    // 312.5 lines. Elite's split timer is counted from that vsync spacing.
    if ((bbc->crtc[8] & 1) && (bbc->video_frames & 1)) {
      bbc->end_of_frame_latched = true;
    } else {
      end_of_frame = true;
    }
  }
  if (end_of_frame) {
    bbc->end_of_main = false;
    bbc->end_of_vert_adjust = false;
    bbc->end_of_frame_latched = false;
    bbc->in_vert_adjust = false;
    VideoEndCharacterRow(bbc);
    VideoEndFrame(bbc);
  }
  bbc->vaddr = bbc->line_start;
}

static void VideoClock(BbcMachine* bbc) {
  int skew = DisplaySkew(bbc);
  bool teletext = Teletext(bbc);
  bool interlace_sync = (bbc->crtc[8] & 1) != 0;
  bool half_hit = bbc->h_count == (((bbc->crtc[0] & 0xff) + 1) >> 1);
  bool vsync_point = !interlace_sync || !bbc->do_even_frame || half_hit;
  bool vsync_ending = false;
  bool vsync_starting = false;
  bool drawing;
  int width = PixelsPerChar(bbc);
  if (skew >= 0 && bbc->h_count == skew) {
    bbc->h_disp = true;
  }
  if (bbc->h_count == (bbc->crtc[1] & 0xff)) {
    bbc->next_line_start = bbc->vaddr;
  }
  if (skew >= 0 && (bbc->h_count == (bbc->crtc[1] + skew) ||
                    bbc->h_count == (bbc->crtc[0] + skew))) {
    bbc->h_disp = false;
  }
  if (bbc->in_vsync && bbc->vpulse == ((bbc->crtc[3] >> 4) & 0x0f) && vsync_point) {
    vsync_ending = true;
    bbc->in_vsync = false;
  }
  if (!bbc->in_vsync && !bbc->had_vsync_row && vsync_point &&
      bbc->v_count == (bbc->crtc[7] & 0x7f)) {
    vsync_starting = true;
    bbc->in_vsync = true;
  }
  if (vsync_starting && !vsync_ending) {
    bbc->had_vsync_row = true;
    bbc->vpulse = 0;
    VideoFlyback(bbc);
  }
  if (vsync_starting || vsync_ending) {
    VideoSetVsyncPin(bbc, bbc->in_vsync);
  }
  drawing = bbc->h_disp && bbc->v_disp && skew >= 0 && bbc->beam_y < BBC_FB_HEIGHT;
  if (drawing) {
    uint16_t ma = (uint16_t)((bbc->vaddr - (uint16_t)skew) & 0x3fff);
    static const uint8_t kBlack[3] = {0, 0, 0};
    int pix;
    bbc->line_drawn = true;
    if (teletext && (bbc->ula_control & 0x10)) {
      for (pix = 0; pix < width; pix++) {
        PutBeam(bbc, bbc->disp_x + pix, bbc->beam_y, kBlack);
        PutBeam(bbc, bbc->disp_x + pix, bbc->beam_y + 1, kBlack);
      }
    } else if (teletext) {
      uint8_t byte = VideoByte(bbc, ma, 0) & 0x7f;
      DrawBeamTeletext(bbc, bbc->disp_x, bbc->beam_y, byte, bbc->scanline);
      MaybeInvertCursor(bbc, bbc->disp_x, bbc->beam_y, width, ma, bbc->scanline);
      MaybeInvertCursor(bbc, bbc->disp_x, bbc->beam_y + 1, width, ma, bbc->scanline);
    } else {
      DrawBeamBitmap(bbc, bbc->disp_x, bbc->beam_y, ma, bbc->scanline);
    }
    bbc->disp_x += width;
  }
  bbc->vaddr = (uint16_t)((bbc->vaddr + 1) & 0x3fff);
  if (bbc->check_vert_adjust) {
    bbc->check_vert_adjust = false;
    if (bbc->end_of_main) {
      if (bbc->vert_adjust == (bbc->crtc[5] & 0x1f)) {
        bbc->end_of_vert_adjust = true;
      }
      bbc->vert_adjust = (bbc->vert_adjust + 1) & 0x1f;
    }
  }
  if (bbc->h_count == 1) {
    if (bbc->v_count == (bbc->crtc[4] & 0x7f) && bbc->scanline == (bbc->crtc[9] & 0x1f)) {
      bbc->end_of_main = true;
      bbc->vert_adjust = 0;
    }
    bbc->check_vert_adjust = true;
  }
  if (bbc->h_count == (bbc->crtc[0] & 0xff)) {
    VideoEndScanline(bbc);
    bbc->h_count = 0;
    bbc->h_disp = DisplaySkew(bbc) == 0;
  } else {
    bbc->h_count = (bbc->h_count + 1) & 0xff;
  }
  if (bbc->v_count == (bbc->crtc[6] & 0x7f) && bbc->v_disp && !bbc->first_scanline) {
    bbc->v_disp = false;
    bbc->video_frames++;
  }
}

static void VideoAdvance(BbcMachine* bbc, int cpu_cycles) {
  int i;
  if (!bbc->beam_started) {
    bbc->beam_started = true;
    bbc->line_start = (uint16_t)(((bbc->crtc[12] << 8) | bbc->crtc[13]) & 0x3fff);
    bbc->next_line_start = bbc->line_start;
    bbc->vaddr = bbc->line_start;
    bbc->h_disp = DisplaySkew(bbc) == 0;
    bbc->v_disp = true;
    bbc->scan_disp = true;
    bbc->first_scanline = true;
    bbc->do_even_frame = true;
    bbc->tt_fg = 7;
  }
  for (i = 0; i < cpu_cycles; i++) {
    bool half = (bbc->ula_control & 0x10) == 0;
    bbc->video_odd = !bbc->video_odd;
    if (half && !bbc->video_odd) {
      continue;
    }
    VideoClock(bbc);
  }
}

int BbcMachineDiscHorizon(const BbcMachine* bbc) {
  int horizon = 1000000;
  if (bbc == NULL) {
    return horizon;
  }
  if (bbc->fdc.delay > 0 && bbc->fdc.delay < horizon) {
    horizon = bbc->fdc.delay;
  }
  if ((bbc->fdc.phase == FDC_WRITE_TRACK || bbc->fdc.phase == FDC_READ_TRACK) &&
      bbc->fdc.track_left > 0 && bbc->fdc.track_left < horizon) {
    horizon = bbc->fdc.track_left;
  }
  return horizon;
}

bool BbcMachineSounding(const BbcMachine* bbc) {
  int i;
  if (bbc == NULL) {
    return false;
  }
  for (i = 0; i < 4; i++) {
    if ((bbc->tone_vol[i] & 15) != 15) {
      return true;
    }
  }
  return false;
}

void BbcMachineSetFast(BbcMachine* bbc, bool fast) {
  if (bbc == NULL || bbc->fast_clock == fast) {
    return;
  }
  bbc->fast_clock = fast;
  bbc->audio_acc = 0;
  if (!fast) {
    bbc->beam_started = false;
    bbc->beam_y = 0;
    bbc->beam_width = 0;
    bbc->beam_size_hold = true;
  }
}

static void DiscService(BbcMachine* bbc);
static void TapeService(BbcMachine* bbc);

void BbcMachineAdvance(BbcMachine* bbc, int cpu_cycles) {
  int via_ticks;
  if (bbc == NULL) {
    return;
  }
  // Polling the disc socket on every instruction steals so much time that the
  // MOS keyboard scan misses keypresses. Look for a new image as soon as one
  // is queued, and check for eject about once a frame.
  if (bbc->disc_listen_started) {
    bool attention;
    bool watch;
    if (cpu_cycles > 0) {
      bbc->disc_watch_acc += cpu_cycles;
    }
    watch = bbc->disc_watch_acc >= 40000;
    attention = __atomic_load_n(&bbc->disc_attention, __ATOMIC_ACQUIRE) != 0;
    if ((watch || attention) &&
        (bbc->fdc.phase == FDC_IDLE || bbc->fdc.phase == FDC_FINISH)) {
      if (watch) {
        bbc->disc_watch_acc = 0;
      }
      __atomic_store_n(&bbc->disc_attention, 0, __ATOMIC_RELAXED);
      DiscService(bbc);
    }
  }
  if (bbc->tape_listen_started) {
    bool attention;
    bool watch;
    if (cpu_cycles > 0) {
      bbc->tape_watch_acc += cpu_cycles;
    }
    watch = bbc->tape_watch_acc >= 40000;
    attention = __atomic_load_n(&bbc->tape_attention, __ATOMIC_ACQUIRE) != 0;
    if (watch || attention) {
      if (watch) {
        bbc->tape_watch_acc = 0;
      }
      __atomic_store_n(&bbc->tape_attention, 0, __ATOMIC_RELAXED);
      TapeService(bbc);
    }
  }
  if (cpu_cycles <= 0) {
    return;
  }
  // The 6522s sit on the 1 MHz bus, so their timers tick once per two CPU cycles.
  bbc->via_phase += cpu_cycles;
  via_ticks = bbc->via_phase / 2;
  ViaAdvance(&bbc->sys, via_ticks);
  ViaAdvance(&bbc->user, via_ticks);
  if (bbc->printer_ack > 0) {
    if (via_ticks >= bbc->printer_ack) {
      bbc->printer_ack = 0;
      UserPrinterAck(bbc);
    } else {
      bbc->printer_ack -= via_ticks;
    }
  }
  bbc->via_phase &= 1;
  AcaiAdvance(bbc, cpu_cycles);
  AdcAdvance(bbc, cpu_cycles);
  FdcAdvance(bbc, cpu_cycles);
  EconetAdvance(bbc, cpu_cycles);
  if (!bbc->fast_clock) {
    bbc->audio_acc += (int64_t)cpu_cycles * BBC_AUDIO_RATE;
    while (bbc->audio_acc >= BBC_CPU_HZ) {
      bbc->audio_acc -= BBC_CPU_HZ;
      SoundEmit(bbc);
    }
  }
  // While the CPU runs ahead, keep the timers and the disc in step but do not
  // paint every pixel. An unprogrammed CRTC has no beam either.
  if (bbc->fast_clock || bbc->crtc[0] == 0) {
    bbc->cycle_acc += cpu_cycles;
    while (bbc->cycle_acc >= BBC_CPU_CYCLES_PER_FRAME) {
      bbc->cycle_acc -= BBC_CPU_CYCLES_PER_FRAME;
      bbc->frames++;
      if (!bbc->fast_clock) {
        bbc->sys.ifr |= 0x02;
      } else {
        bbc->beam_frames++;
        bbc->sys.ifr |= 0x02;
      }
    }
    return;
  }
  VideoAdvance(bbc, cpu_cycles);
}

int BbcMachineCompletedFrames(const BbcMachine* bbc) {
  return bbc->beam_frames;
}

bool BbcMachineIrqPending(const BbcMachine* bbc) {
  // ACCCON bit IRR forces the CPU IRQ line.
  if (bbc->master && (bbc->acccon & 0x80) != 0) {
    return true;
  }
  if (ViaIrq(&bbc->sys) || ViaIrq(&bbc->user) || AcaiIrq(bbc) || HdIrqPending(bbc)) {
    return true;
  }
  return bbc->tube != NULL && TubeHostIrq(bbc->tube);
}

int BbcFrameWidth(const BbcMachine* bbc) { return bbc->frame_width; }
int BbcFrameHeight(const BbcMachine* bbc) { return bbc->frame_height; }
const uint8_t* BbcFramebuffer(const BbcMachine* bbc) { return bbc->fb; }

void BbcPixel(const BbcMachine* bbc, int x, int y, uint8_t rgb[3]) {
  if (x < 0 || y < 0 || x >= bbc->frame_width || y >= bbc->frame_height) {
    rgb[0] = rgb[1] = rgb[2] = 0;
    return;
  }
  const uint8_t* p = bbc->fb + ((size_t)y * BBC_FB_WIDTH + (size_t)x) * 3;
  rgb[0] = p[0];
  rgb[1] = p[1];
  rgb[2] = p[2];
}

bool BbcMachineWritePpm(BbcMachine* bbc, const char* path) {
  FILE* fp;
  int y;
  if (path == NULL) {
    return false;
  }
  BbcMachineRender(bbc);
  fp = fopen(path, "wb");
  if (fp == NULL) {
    return false;
  }
  fprintf(fp, "P6\n%d %d\n255\n", bbc->frame_width, bbc->frame_height);
  for (y = 0; y < bbc->frame_height; y++) {
    const uint8_t* row = bbc->fb + (size_t)y * BBC_FB_WIDTH * 3;
    if (fwrite(row, 3, (size_t)bbc->frame_width, fp) != (size_t)bbc->frame_width) {
      fclose(fp);
      return false;
    }
  }
  fclose(fp);
  return true;
}

static bool ReadWholeFile(const char* path, uint8_t** out, size_t* length) {
  char expanded[PATH_MAX];
  FILE* fp;
  // The shell does not expand ~ after the comma in -rom 15,~/file.
  if (path != NULL && path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
    const char* home = HostHomeDirectory();
    int written;
    if (home != NULL && home[0] != '\0') {
      written = snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
      if (written > 0 && (size_t)written < sizeof(expanded)) {
        path = expanded;
      }
    }
  }
  fp = fopen(path, "rb");
  long size;
  uint8_t* buf;
  if (fp == NULL) {
    return false;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return false;
  }
  size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return false;
  }
  rewind(fp);
  buf = malloc((size_t)size + 1);
  if (buf == NULL) {
    fclose(fp);
    return false;
  }
  if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
    free(buf);
    fclose(fp);
    return false;
  }
  fclose(fp);
  *out = buf;
  *length = (size_t)size;
  return true;
}

bool BbcMachineLoadOs(BbcMachine* bbc, const char* path) {
  uint8_t* buf = NULL;
  size_t length = 0;
  size_t i;
  size_t n;
  if (bbc->ram == NULL || path == NULL) {
    return false;
  }
  if (!ReadWholeFile(path, &buf, &length)) {
    fprintf(stderr, "Unable to read BBC OS ROM '%s'\n", path);
    return false;
  }
  n = length > 16384 ? 16384 : length;
  if (bbc->master && bbc->mos_low == NULL && !EnsureMasterRam(bbc)) {
    free(buf);
    return false;
  }
  if (bbc->master) {
    if (bbc->os_rom == NULL) {
      bbc->os_rom = malloc(16384);
      if (bbc->os_rom == NULL) {
        free(buf);
        return false;
      }
      memset(bbc->os_rom, 0xff, 16384);
    }
    memcpy(bbc->os_rom, buf, n);
  }
  for (i = 0; i < n; i++) {
    uint16_t addr = (uint16_t)(0xc000 + i);
    if (addr >= 0xfc00 && addr <= 0xfeff) {
      continue;
    }
    if (bbc->master && addr < 0xe000) {
      bbc->mos_low[i] = buf[i];
      if (!bbc->hazel_mapped) {
        bbc->ram[addr] = buf[i];
      }
    } else {
      bbc->ram[addr] = buf[i];
    }
  }
  if (bbc->master) {
    bbc->mos_saved = true;
  }
  free(buf);
  return true;
}

static bool ExtIs(const char* path, const char* ext) {
  size_t path_n;
  size_t ext_n;
  size_t i;
  if (path == NULL || ext == NULL) {
    return false;
  }
  path_n = strlen(path);
  ext_n = strlen(ext);
  if (path_n < ext_n) {
    return false;
  }
  for (i = 0; i < ext_n; i++) {
    char ch = path[path_n - ext_n + i];
    if (ch >= 'A' && ch <= 'Z') {
      ch = (char)(ch - 'A' + 'a');
    }
    if (ch != ext[i]) {
      return false;
    }
  }
  return true;
}

static bool RomHas(const uint8_t* rom, const char* text) {
  size_t n;
  int i;
  if (rom == NULL || text == NULL) {
    return false;
  }
  n = strlen(text);
  if (n == 0 || n > 16384) {
    return false;
  }
  for (i = 0; i + (int)n <= 16384; i++) {
    if (memcmp(rom + i, text, n) == 0) {
      return true;
    }
  }
  return false;
}

static void DiscShape(const char* path, size_t length, int* spt, int* sides, int* tracks) {
  int unit;
  if (ExtIs(path, ".adl")) {
    *spt = 16;
    *sides = 2;
  } else if (ExtIs(path, ".adf") || ExtIs(path, ".adm")) {
    *spt = 16;
    *sides = 1;
  } else if (ExtIs(path, ".dsd")) {
    *spt = 10;
    *sides = 2;
  } else {
    *spt = 10;
    *sides = 1;
  }
  unit = (*spt) * (*sides) * 256;
  *tracks = 0;
  if (unit > 0) {
    *tracks = (int)(length / (size_t)unit);
    // Keep a final track that the file only partly fills. Reads past the
    // file return zero.
    if (length % (size_t)unit != 0 && *tracks < 80) {
      (*tracks)++;
    }
  }
  if (*tracks > 80) {
    *tracks = 80;
  }
}

static void FdcDetect(BbcMachine* bbc) {
  int i;
  if (bbc->fdc.forced) {
    return;
  }
  bbc->fdc.kind = BBC_FDC_8271;
  // A fitted ADFS or DFS ROM stays inactive until that ROM uses the drive.
  // An ADFS image still selects the 1770, because that is the controller it
  // needs before any ROM has run.
  for (i = 0; i < 2; i++) {
    const char* paths[2];
    int p;
    paths[0] = bbc->fdc.disc[i].path;
    paths[1] = bbc->fdc.extra[i].path;
    for (p = 0; p < 2; p++) {
      if (paths[p] != NULL && (ExtIs(paths[p], ".adf") || ExtIs(paths[p], ".adm") ||
                               ExtIs(paths[p], ".adl"))) {
        bbc->fdc.kind = BBC_FDC_1770;
        return;
      }
    }
  }
}

static void DiscRelease(BbcDiscImage* image) {
  if (image == NULL) {
    return;
  }
  if (image->fd >= 0) {
    shutdown(image->fd, SHUT_RDWR);
    SocketClose(image->fd);
    image->fd = -1;
  }
  free(image->data);
  free(image->path);
  memset(image, 0, sizeof(*image));
  image->fd = -1;
}

static void DiscDescribe(int drive, const char* name, int tracks, int sides, int spt) {
  fprintf(stderr, "Disc %d: %s (%d track%s, %d side%s, %s)\n", drive, name, tracks,
          tracks == 1 ? "" : "s", sides, sides == 1 ? "" : "s", spt >= 16 ? "ADFS" : "DFS");
}

// On success the image owns data and fd. On failure the caller still owns both.
static bool DiscMount(BbcMachine* bbc, int drive, uint8_t* data, size_t length, const char* name,
                      bool protect, int fd) {
  int spt = 10;
  int sides = 1;
  int tracks = 0;
  BbcDiscImage* slot;
  char* stored;
  if (bbc == NULL || data == NULL || name == NULL || drive < 0 || drive > 3) {
    return false;
  }
  DiscShape(name, length, &spt, &sides, &tracks);
  if (drive >= 2 && (sides > 1 || spt >= 16)) {
    return false;
  }
  if (drive >= 2 && bbc->fdc.disc[drive - 2].data != NULL && bbc->fdc.disc[drive - 2].sides > 1) {
    return false;
  }
  stored = strdup(name);
  if (stored == NULL) {
    return false;
  }
  if (drive >= 2) {
    slot = &bbc->fdc.extra[drive - 2];
  } else {
    slot = &bbc->fdc.disc[drive];
    if (sides > 1) {
      DiscRelease(&bbc->fdc.extra[drive]);
    }
  }
  DiscRelease(slot);
  slot->data = data;
  slot->length = length;
  slot->spt = spt;
  slot->sides = sides;
  slot->tracks = tracks;
  slot->protect = protect;
  slot->path = stored;
  slot->fd = fd;
  FdcDetect(bbc);
  DiscDescribe(drive, stored, tracks, sides, spt);
  return true;
}

bool BbcMachineLoadDisc(BbcMachine* bbc, int drive, const char* path) {
  char expanded[PATH_MAX];
  uint8_t* buf = NULL;
  size_t length = 0;
  const char* stored;
  if (bbc == NULL || path == NULL || drive < 0 || drive > 1) {
    return false;
  }
  stored = path;
  if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
    const char* home = HostHomeDirectory();
    int written;
    if (home != NULL && home[0] != '\0') {
      written = snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
      if (written > 0 && (size_t)written < sizeof(expanded)) {
        stored = expanded;
      }
    }
  }
  if (!ReadWholeFile(stored, &buf, &length)) {
    fprintf(stderr, "Unable to read disc image '%s'\n", path);
    return false;
  }
  if (!DiscMount(bbc, drive, buf, length, stored, access(stored, W_OK) != 0, -1)) {
    free(buf);
    return false;
  }
  return true;
}

struct DiscPull {
  BbcMachine* bbc;
  int fd;
};

static int DiscPullRead(void* ctx, void* buf, size_t n) {
  struct DiscPull* pull = (struct DiscPull*)ctx;
  uint8_t* bytes = (uint8_t*)buf;
  size_t off = 0;
  while (off < n) {
    struct pollfd pfd;
    int ready;
    bool stop;
    ssize_t got;
    pthread_mutex_lock(&pull->bbc->disc_mu);
    stop = pull->bbc->disc_listen_stop;
    pthread_mutex_unlock(&pull->bbc->disc_mu);
    if (stop) {
      return -1;
    }
    pfd.fd = pull->fd;
    pfd.events = POLLIN;
    ready = SocketPoll(&pfd, 1, 200);
    if (ready < 0) {
      if (SocketInterrupted()) {
        continue;
      }
      return -1;
    }
    if (ready == 0) {
      continue;
    }
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
      return -1;
    }
    if ((pfd.revents & (POLLIN | POLLHUP)) == 0) {
      continue;
    }
    got = SocketRead(pull->fd, bytes + off, n - off);
    if (got < 0) {
      if (SocketInterrupted()) {
        continue;
      }
      return -1;
    }
    if (got == 0) {
      return -1;
    }
    off += (size_t)got;
  }
  return 0;
}

static void DiscEnqueue(BbcMachine* bbc, int fd, CumanaInsert* insert) {
  struct CumanaPending* item = calloc(1, sizeof(*item));
  struct CumanaPending* tail;
  if (item == NULL) {
    CumanaSendReply(fd, CUMANA_ERR, -1, "out of memory");
    CumanaInsertFree(insert);
    SocketClose(fd);
    return;
  }
  item->fd = fd;
  item->insert = *insert;
  memset(insert, 0, sizeof(*insert));
  pthread_mutex_lock(&bbc->disc_mu);
  tail = bbc->disc_pending;
  if (tail == NULL) {
    bbc->disc_pending = item;
  } else {
    while (tail->next != NULL) {
      tail = tail->next;
    }
    tail->next = item;
  }
  __atomic_store_n(&bbc->disc_attention, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&bbc->disc_mu);
}

static void* DiscListenMain(void* arg) {
  BbcMachine* bbc = (BbcMachine*)arg;
  for (;;) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd;
    bool stop;
    pthread_mutex_lock(&bbc->disc_mu);
    stop = bbc->disc_listen_stop;
    pthread_mutex_unlock(&bbc->disc_mu);
    if (stop) {
      break;
    }
    {
      struct pollfd listen_pfd;
      int ready;
      listen_pfd.fd = bbc->disc_listen_fd;
      listen_pfd.events = POLLIN;
      ready = SocketPoll(&listen_pfd, 1, 200);
      if (ready < 0) {
        if (SocketInterrupted()) {
          continue;
        }
        break;
      }
      if (ready == 0) {
        continue;
      }
    }
    fd = (int)accept(bbc->disc_listen_fd, (struct sockaddr*)&addr, &len);
    if (fd < 0) {
      if (SocketWouldBlock()) {
        continue;
      }
      pthread_mutex_lock(&bbc->disc_mu);
      stop = bbc->disc_listen_stop;
      pthread_mutex_unlock(&bbc->disc_mu);
      if (stop) {
        break;
      }
      continue;
    }
    {
      int one = 1;
#ifdef SO_NOSIGPIPE
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    }
    pthread_mutex_lock(&bbc->disc_mu);
    bbc->disc_handshake_fd = fd;
    stop = bbc->disc_listen_stop;
    pthread_mutex_unlock(&bbc->disc_mu);
    if (stop) {
      SocketClose(fd);
      break;
    }
    {
      struct DiscPull pull;
      CumanaInsert insert;
      pull.bbc = bbc;
      pull.fd = fd;
      if (CumanaReadInsert(DiscPullRead, &pull, &insert) != 0) {
        SocketClose(fd);
      } else if (insert.drive > 3) {
        CumanaSendReply(fd, CUMANA_ERR, -1, "drive must be 0, 1, 2, or 3");
        CumanaInsertFree(&insert);
        SocketClose(fd);
      } else {
        DiscEnqueue(bbc, fd, &insert);
      }
    }
    pthread_mutex_lock(&bbc->disc_mu);
    if (bbc->disc_handshake_fd == fd) {
      bbc->disc_handshake_fd = -1;
    }
    pthread_mutex_unlock(&bbc->disc_mu);
  }
  return NULL;
}

static void DiscDropPending(struct CumanaPending* pending) {
  while (pending != NULL) {
    struct CumanaPending* next = pending->next;
    if (pending->fd >= 0) {
      SocketClose(pending->fd);
    }
    CumanaInsertFree(&pending->insert);
    free(pending);
    pending = next;
  }
}

static void DiscStop(BbcMachine* bbc) {
  struct CumanaPending* pending;
  int handshake;
  if (bbc == NULL || !bbc->disc_listen_started) {
    return;
  }
  pthread_mutex_lock(&bbc->disc_mu);
  bbc->disc_listen_stop = true;
  handshake = bbc->disc_handshake_fd;
  pthread_mutex_unlock(&bbc->disc_mu);
  if (bbc->disc_listen_fd >= 0) {
    shutdown(bbc->disc_listen_fd, SHUT_RDWR);
  }
  if (handshake >= 0) {
    shutdown(handshake, SHUT_RDWR);
  }
  pthread_join(bbc->disc_listen_thread, NULL);
  if (bbc->disc_listen_fd >= 0) {
    SocketClose(bbc->disc_listen_fd);
    bbc->disc_listen_fd = -1;
  }
  pthread_mutex_lock(&bbc->disc_mu);
  pending = bbc->disc_pending;
  bbc->disc_pending = NULL;
  pthread_mutex_unlock(&bbc->disc_mu);
  DiscDropPending(pending);
  pthread_mutex_destroy(&bbc->disc_mu);
  bbc->disc_listen_started = false;
}

int BbcMachineListenDiscs(BbcMachine* bbc, int port) {
  struct sockaddr_in addr;
  socklen_t len;
  int fd;
  int one = 1;
  if (bbc == NULL || bbc->disc_listen_started || port < 0 || port > 65535) {
    return -1;
  }
  if (!SocketStartup()) {
    return -1;
  }
  fd = (int)socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  // On Windows SO_REUSEADDR lets a second listener take a port already in
  // use, so a second emulator would steal the disc socket.
#ifdef _WIN32
  setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
#else
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
    SocketClose(fd);
    return -1;
  }
  len = sizeof(addr);
  if (getsockname(fd, (struct sockaddr*)&addr, &len) != 0) {
    SocketClose(fd);
    return -1;
  }
  if (pthread_mutex_init(&bbc->disc_mu, NULL) != 0) {
    SocketClose(fd);
    return -1;
  }
  bbc->disc_listen_fd = fd;
  bbc->disc_listen_port = ntohs(addr.sin_port);
  bbc->disc_handshake_fd = -1;
  bbc->disc_listen_stop = false;
  bbc->disc_pending = NULL;
  bbc->disc_listen_started = true;
  if (pthread_create(&bbc->disc_listen_thread, NULL, DiscListenMain, bbc) != 0) {
    bbc->disc_listen_started = false;
    pthread_mutex_destroy(&bbc->disc_mu);
    SocketClose(fd);
    bbc->disc_listen_fd = -1;
    return -1;
  }
  return bbc->disc_listen_port;
}

static bool DiscSlotFree(const BbcMachine* bbc, int drive) {
  if (drive < 0 || drive > 3) {
    return false;
  }
  if (drive >= 2) {
    if (bbc->fdc.extra[drive - 2].data != NULL) {
      return false;
    }
    if (bbc->fdc.disc[drive - 2].data != NULL && bbc->fdc.disc[drive - 2].sides > 1) {
      return false;
    }
    return true;
  }
  return bbc->fdc.disc[drive].data == NULL;
}

static int DiscChoose(const BbcMachine* bbc, const CumanaInsert* insert, const char** error) {
  int spt = 10;
  int sides = 1;
  int tracks = 0;
  int drive = insert->drive;
  int limit;
  int i;
  *error = "disc not inserted";
  DiscShape(insert->name != NULL ? insert->name : "", insert->length, &spt, &sides, &tracks);
  (void)tracks;
  if (drive > 3) {
    *error = "drive must be 0, 1, 2, or 3";
    return -1;
  }
  if (drive >= 2 && sides > 1) {
    *error = "a double-sided image uses drive 0 or 1";
    return -1;
  }
  if (drive >= 2 && spt >= 16) {
    *error = "an ADFS image uses drive 0 or 1";
    return -1;
  }
  if (drive >= 2 && bbc->fdc.disc[drive - 2].data != NULL && bbc->fdc.disc[drive - 2].sides > 1) {
    *error = "that side is already part of a double-sided image";
    return -1;
  }
  if (drive >= 0) {
    return drive;
  }
  limit = (sides > 1 || spt >= 16) ? 2 : 4;
  for (i = 0; i < limit; i++) {
    if (DiscSlotFree(bbc, i)) {
      return i;
    }
  }
  return 0;
}

static void DiscWatch(BbcDiscImage* image) {
  struct pollfd pfd;
  char drain[32];
  ssize_t n;
  if (image == NULL || image->fd < 0) {
    return;
  }
  pfd.fd = image->fd;
  pfd.events = POLLIN;
  if (SocketPoll(&pfd, 1, 0) <= 0) {
    return;
  }
  if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
    return;
  }
  n = SocketRead(image->fd, drain, sizeof(drain));
  if (n == 0 || (n < 0 && !SocketWouldBlock())) {
    DiscRelease(image);
  }
}

static void DiscService(BbcMachine* bbc) {
  struct CumanaPending* pending;
  if (bbc == NULL || !bbc->disc_listen_started) {
    return;
  }
  if (bbc->fdc.phase != FDC_IDLE && bbc->fdc.phase != FDC_FINISH) {
    __atomic_store_n(&bbc->disc_attention, 1, __ATOMIC_RELAXED);
    return;
  }
  DiscWatch(&bbc->fdc.disc[0]);
  DiscWatch(&bbc->fdc.disc[1]);
  DiscWatch(&bbc->fdc.extra[0]);
  DiscWatch(&bbc->fdc.extra[1]);
  pthread_mutex_lock(&bbc->disc_mu);
  pending = bbc->disc_pending;
  bbc->disc_pending = NULL;
  pthread_mutex_unlock(&bbc->disc_mu);
  while (pending != NULL) {
    struct CumanaPending* next = pending->next;
    const char* error = NULL;
    int drive;
    int fd = pending->fd;
    uint8_t* data = pending->insert.data;
    size_t length = pending->insert.length;
    const char* name = pending->insert.name != NULL ? pending->insert.name : "disc";
    bool protect = pending->insert.protect != 0;
    pending->fd = -1;
    pending->insert.data = NULL;
    drive = DiscChoose(bbc, &pending->insert, &error);
    if (drive < 0) {
      CumanaSendReply(fd, CUMANA_ERR, -1, error != NULL ? error : "disc not inserted");
      SocketClose(fd);
      free(data);
    } else if (!DiscMount(bbc, drive, data, length, name, protect, fd)) {
      CumanaSendReply(fd, CUMANA_ERR, -1, "disc not inserted");
      SocketClose(fd);
      free(data);
    } else if (CumanaSendReply(fd, CUMANA_OK, drive, "inserted") != 0) {
      DiscRelease(drive >= 2 ? &bbc->fdc.extra[drive - 2] : &bbc->fdc.disc[drive]);
    }
    CumanaInsertFree(&pending->insert);
    free(pending);
    pending = next;
  }
}

void BbcMachineSetFdc(BbcMachine* bbc, int kind) {
  if (bbc == NULL || (kind != BBC_FDC_8271 && kind != BBC_FDC_1770)) {
    return;
  }
  bbc->fdc.kind = kind;
  bbc->fdc.forced = true;
  FdcResetChip(bbc);
}

void BbcMachineResetFdc(BbcMachine* bbc) {
  if (bbc != NULL) {
    FdcResetChip(bbc);
  }
}

bool BbcMachineNmiPending(const BbcMachine* bbc) {
  return bbc != NULL && bbc->nmi_edge;
}

void BbcMachineClearNmi(BbcMachine* bbc) {
  if (bbc != NULL) {
    bbc->nmi_edge = false;
  }
}

static void NoteRomFiling(BbcMachine* bbc, int slot, const char* name);

bool BbcMachineLoadSidewaysBytes(BbcMachine* bbc, int slot, const uint8_t* bytes,
                                 size_t length) {
  if (slot < 0 || slot > 15) {
    return false;
  }
  if (bbc->sideways[slot] == NULL) {
    bbc->sideways[slot] = malloc(16384);
    if (bbc->sideways[slot] == NULL) {
      return false;
    }
  }
  memset(bbc->sideways[slot], 0xff, 16384);
  if (bytes != NULL && length > 0) {
    if (length > 16384) {
      length = 16384;
    }
    memcpy(bbc->sideways[slot], bytes, length);
  }
  bbc->sideways_loaded[slot] = true;
  bbc->any_sideways = true;
  NoteRomFiling(bbc, slot, NULL);
  // The new image is already in the buffer. Dropping the mapped slot skips
  // the write-back that would replace it with the old window.
  if (bbc->ram != NULL && bbc->mapped_slot == slot && bbc->mapped_virt == 0) {
    bbc->mapped_slot = -1;
    bbc->mapped_virt = -1;
  }
  MapRomsel(bbc);
  FdcDetect(bbc);
  return true;
}

bool BbcMachineLoadSideways(BbcMachine* bbc, int slot, const char* path) {
  uint8_t* buf = NULL;
  size_t length = 0;
  bool ok;
  if (!ReadWholeFile(path, &buf, &length)) {
    fprintf(stderr, "Unable to read sideways ROM '%s'\n", path);
    return false;
  }
  ok = BbcMachineLoadSidewaysBytes(bbc, slot, buf, length);
  free(buf);
  if (ok) {
    NoteRomFiling(bbc, slot, path);
  }
  return ok;
}

// Virtual images 1-7 share the socket with image 0 and are selected by
// bits 4-6 of ROMSEL. They stay ROM, and they do not change which filing
// system the socket claims.
static bool LoadSidewaysVirtual(BbcMachine* bbc, int slot, int virt, const char* path) {
  uint8_t* buf = NULL;
  size_t length = 0;
  uint8_t** image;
  if (bbc == NULL || slot < 0 || slot > 15 || virt < 1 || virt > 7) {
    return false;
  }
  if (!ReadWholeFile(path, &buf, &length)) {
    fprintf(stderr, "Unable to read sideways ROM '%s'\n", path);
    return false;
  }
  image = &bbc->sideways_page[slot][virt - 1];
  if (*image == NULL) {
    *image = malloc(16384);
    if (*image == NULL) {
      free(buf);
      return false;
    }
  }
  memset(*image, 0xff, 16384);
  if (length > 0) {
    if (length > 16384) {
      length = 16384;
    }
    memcpy(*image, buf, length);
  }
  free(buf);
  bbc->any_sideways = true;
  if (bbc->ram != NULL && bbc->mapped_slot == slot && bbc->mapped_virt == virt) {
    bbc->mapped_slot = -1;
    bbc->mapped_virt = -1;
  }
  MapRomsel(bbc);
  return true;
}

static bool DirectoryExists(const char* path) {
  struct stat st;
  return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool EndsWithIgnoreCase(const char* text, const char* suffix) {
  size_t text_n = strlen(text);
  size_t suffix_n = strlen(suffix);
  size_t i;
  if (text_n < suffix_n) {
    return false;
  }
  for (i = 0; i < suffix_n; i++) {
    char a = text[text_n - suffix_n + i];
    char b = suffix[i];
    if (tolower((unsigned char)a) != tolower((unsigned char)b)) {
      return false;
    }
  }
  return true;
}

static bool EqualsIgnoreCase(const char* text, const char* other) {
  size_t i;
  for (i = 0; text[i] != '\0' && other[i] != '\0'; i++) {
    if (tolower((unsigned char)text[i]) != tolower((unsigned char)other[i])) {
      return false;
    }
  }
  return text[i] == '\0' && other[i] == '\0';
}

// os.rom and os-<name>.rom are the MOS. <0-15>.rom and <0-15>-<name>.rom
// are virtual image 0. <0-15>.<0-7>-<name>.rom is an image in that socket.
// Other .rom names are rejected.
static int ClassifyRomName(const char* name, int* slot, int* virt) {
  char stem[256];
  size_t length;
  size_t i;
  int value;
  int digits;
  if (name == NULL || name[0] == '.' || !EndsWithIgnoreCase(name, ".rom")) {
    return 0;
  }
  length = strlen(name);
  if (length <= 4 || length - 4 >= sizeof(stem)) {
    return -1;
  }
  memcpy(stem, name, length - 4);
  stem[length - 4] = '\0';
  if (EqualsIgnoreCase(stem, "os")) {
    *slot = -1;
    *virt = 0;
    return 1;
  }
  if (strncasecmp(stem, "os-", 3) == 0 && stem[3] != '\0') {
    *slot = -1;
    *virt = 0;
    return 1;
  }
  if (!isdigit((unsigned char)stem[0])) {
    return -1;
  }
  value = 0;
  digits = 0;
  for (i = 0; isdigit((unsigned char)stem[i]) && digits < 2; i++) {
    value = value * 10 + (stem[i] - '0');
    digits++;
  }
  if (isdigit((unsigned char)stem[i]) || value > 15) {
    return -1;
  }
  if (stem[i] == '.') {
    int virtual = 0;
    int vdigits = 0;
    i++;
    if (!isdigit((unsigned char)stem[i])) {
      return -1;
    }
    for (; isdigit((unsigned char)stem[i]) && vdigits < 2; i++) {
      virtual = virtual * 10 + (stem[i] - '0');
      vdigits++;
    }
    if (isdigit((unsigned char)stem[i]) || virtual > 7) {
      return -1;
    }
    if (stem[i] != '-' || stem[i + 1] == '\0') {
      return -1;
    }
    *slot = value;
    *virt = virtual;
    return 1;
  }
  if (stem[i] == '\0' || (stem[i] == '-' && stem[i + 1] != '\0')) {
    *slot = value;
    *virt = 0;
    return 1;
  }
  return -1;
}

static char* JoinPath(const char* dir, const char* name) {
  size_t bytes = strlen(dir) + 1 + strlen(name) + 1;
  char* path = malloc(bytes);
  if (path == NULL) {
    return NULL;
  }
  snprintf(path, bytes, "%s/%s", dir, name);
  return path;
}

// Windows paths may use either separator.
static const char* LastSeparator(const char* path) {
  const char* slash = strrchr(path, '/');
#ifdef _WIN32
  const char* backslash = strrchr(path, '\\');
  if (backslash != NULL && (slash == NULL || backslash > slash)) {
    slash = backslash;
  }
#endif
  return slash;
}

static char* RomDirectoryBeside(const char* binary, const char* directory) {
  char folder[PATH_MAX];
  char candidate[PATH_MAX];
  const char* slash;
  size_t folder_len;
  if (binary == NULL || binary[0] == '\0' || directory == NULL) {
    return NULL;
  }
  slash = LastSeparator(binary);
  if (slash == NULL || slash == binary) {
    return NULL;
  }
  folder_len = (size_t)(slash - binary);
  if (folder_len >= sizeof(folder)) {
    return NULL;
  }
  memcpy(folder, binary, folder_len);
  folder[folder_len] = '\0';
  snprintf(candidate, sizeof(candidate), "%s/%s", folder, directory);
  if (DirectoryExists(candidate)) {
    return strdup(candidate);
  }
  slash = LastSeparator(folder);
  if (slash == NULL) {
    return NULL;
  }
  if (slash == folder) {
    snprintf(candidate, sizeof(candidate), "/%s", directory);
  } else {
    snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)(slash - folder), folder, directory);
  }
  if (DirectoryExists(candidate)) {
    return strdup(candidate);
  }
  // The executable of a Mac app lives in Something.app/Contents/MacOS.
  // Socket directories may sit in Contents/Resources, or next to the .app.
  {
    const char* scan = folder;
    const char* macos = NULL;
    const char* hit;
    while ((hit = strstr(scan, ".app/Contents/MacOS")) != NULL) {
      macos = hit;
      scan = hit + 1;
    }
    if (macos != NULL && macos[strlen(".app/Contents/MacOS")] == '\0') {
      char app[PATH_MAX];
      size_t app_len = (size_t)(macos - folder) + 4;
      if (app_len < sizeof(app)) {
        memcpy(app, folder, app_len);
        app[app_len] = '\0';
        snprintf(candidate, sizeof(candidate), "%s/Contents/Resources/%s", app, directory);
        if (DirectoryExists(candidate)) {
          return strdup(candidate);
        }
        slash = LastSeparator(app);
        if (slash == app) {
          snprintf(candidate, sizeof(candidate), "/%s", directory);
        } else if (slash != NULL) {
          snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)(slash - app), app, directory);
        } else {
          candidate[0] = '\0';
        }
        if (candidate[0] != '\0' && DirectoryExists(candidate)) {
          return strdup(candidate);
        }
      }
    }
  }
  return NULL;
}

const char* BbcMachineRomDirectoryName(int model) {
  if (model == BBC_MACHINE_MASTER || model == BBC_MACHINE_MASTER256) {
    return BBC_MASTER_ROM_DIRECTORY;
  }
  return BBC_B_ROM_DIRECTORY;
}

char* BbcMachineFindRomDirectory(const char* argv0, const char* directory) {
  char* found;
  if (directory == NULL || directory[0] == '\0') {
    directory = BBC_B_ROM_DIRECTORY;
  }
  if (DirectoryExists(directory)) {
    return strdup(directory);
  }
  found = RomDirectoryBeside(argv0, directory);
  if (found != NULL) {
    return found;
  }
  return NULL;
}

int BbcMachineParseModel(const char* text) {
  if (text == NULL) {
    return -1;
  }
  if (EqualsIgnoreCase(text, "b") || EqualsIgnoreCase(text, "modelb") ||
      EqualsIgnoreCase(text, "bbcb")) {
    return BBC_MACHINE_B;
  }
  if (EqualsIgnoreCase(text, "master") || EqualsIgnoreCase(text, "master128")) {
    return BBC_MACHINE_MASTER;
  }
  if (EqualsIgnoreCase(text, "master256")) {
    return BBC_MACHINE_MASTER256;
  }
  return -1;
}

static bool ParseSocketNumber(const char* text, int* socket) {
  char* end = NULL;
  long value;
  if (text == NULL || text[0] == '\0') {
    return false;
  }
  value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 0 || value > 15) {
    return false;
  }
  *socket = (int)value;
  return true;
}

bool BbcMachineParseSidewaysRam(const char* text, bool ram[16]) {
  char buf[128];
  char* cursor;
  size_t length;
  int i;
  if (text == NULL || ram == NULL) {
    return false;
  }
  memset(ram, 0, 16);
  if (strcmp(text, "4") == 0) {
    for (i = 4; i <= 7; i++) {
      ram[i] = true;
    }
    return true;
  }
  if (strcmp(text, "8") == 0) {
    for (i = 0; i < 8; i++) {
      ram[i] = true;
    }
    return true;
  }
  if (strcmp(text, "16") == 0 || EqualsIgnoreCase(text, "all")) {
    for (i = 0; i < 16; i++) {
      ram[i] = true;
    }
    return true;
  }
  length = strlen(text);
  if (length == 0 || length >= sizeof(buf)) {
    return false;
  }
  memcpy(buf, text, length + 1);
  cursor = buf;
  while (*cursor != '\0') {
    char* comma = strchr(cursor, ',');
    char* dash;
    if (comma != NULL) {
      *comma = '\0';
    }
    dash = strchr(cursor, '-');
    if (dash != NULL) {
      int first;
      int last;
      int socket;
      *dash = '\0';
      if (!ParseSocketNumber(cursor, &first) || !ParseSocketNumber(dash + 1, &last) || first > last) {
        return false;
      }
      for (socket = first; socket <= last; socket++) {
        ram[socket] = true;
      }
    } else {
      int socket;
      if (!ParseSocketNumber(cursor, &socket)) {
        return false;
      }
      ram[socket] = true;
    }
    if (comma == NULL) {
      break;
    }
    cursor = comma + 1;
  }
  return true;
}

bool BbcMachineSetSidewaysRam(BbcMachine* bbc, const bool ram[16]) {
  int i;
  if (bbc == NULL || ram == NULL) {
    return false;
  }
  CommitSideways(bbc);
  for (i = 0; i < 16; i++) {
    bbc->sideways_ram[i] = ram[i];
    if (!ram[i] || bbc->sideways[i] != NULL) {
      continue;
    }
    bbc->sideways[i] = calloc(16384, 1);
    if (bbc->sideways[i] == NULL) {
      return false;
    }
  }
  bbc->mapped_slot = -1;
  bbc->mapped_virt = -1;
  MapRomsel(bbc);
  return true;
}

bool BbcMachineIsMaster(const BbcMachine* bbc) { return bbc != NULL && bbc->master; }

bool BbcMachineSetModel(BbcMachine* bbc, int model) {
  bool ram[16];
  bool want_master;
  int i;
  if (bbc == NULL || (model != BBC_MACHINE_B && model != BBC_MACHINE_MASTER &&
                      model != BBC_MACHINE_MASTER256)) {
    return false;
  }
  want_master = model != BBC_MACHINE_B;
  if (bbc->master && !want_master) {
    WriteAcccon(bbc, 0);
    bbc->romsel = (uint8_t)(bbc->romsel & 0x0f);
    MapRomsel(bbc);
  }
  bbc->model = model;
  bbc->master = want_master;
  if (want_master) {
    // A flat battery reads as zeros, and a zero checksum selects ROM 0.
    // These are the user bytes MOS 3.50 keeps: language 12 (BASIC) and
    // filing system 13 (ADFS). MOS replaces them if its own checksum fails.
    static const uint8_t kCmosUser[] = {
        0x00, 0xfe, 0x00, 0xeb, 0x00, 0xcd, 0xff, 0xff, 0x15, 0x00, 0x17,
        0x48, 0x32, 0x08, 0x0a, 0x2d, 0x82,
    };
    int byte;
    bool blank = true;
    if (!EnsureMasterRam(bbc)) {
      return false;
    }
    for (byte = 0; byte < (int)sizeof(kCmosUser); byte++) {
      if (bbc->cmos[14 + byte] != 0) {
        blank = false;
        break;
      }
    }
    if (blank) {
      memcpy(bbc->cmos + 14, kCmosUser, sizeof(kCmosUser));
    }
    BbcMachineSetFdc(bbc, BBC_FDC_1770);
    if (bbc->ram != NULL && !bbc->mos_saved && !bbc->hazel_mapped) {
      memcpy(bbc->mos_low, bbc->ram + 0xc000, 0x2000);
      bbc->mos_saved = true;
    }
  }
  memset(ram, 0, sizeof(ram));
  if (model == BBC_MACHINE_MASTER) {
    for (i = 4; i <= 7; i++) {
      ram[i] = true;
    }
  } else if (model == BBC_MACHINE_MASTER256) {
    for (i = 0; i < 16; i++) {
      ram[i] = true;
    }
  }
  return BbcMachineSetSidewaysRam(bbc, ram);
}

void BbcRomFileFree(BbcRomFile* files, int count) {
  int i;
  if (files == NULL || count <= 0) {
    return;
  }
  for (i = 0; i < count; i++) {
    free(files[i].path);
    files[i].path = NULL;
  }
}

static bool ContainsIgnoreCase(const char* text, const char* needle) {
  size_t needle_n;
  size_t i;
  if (text == NULL || needle == NULL) {
    return false;
  }
  needle_n = strlen(needle);
  if (needle_n == 0) {
    return false;
  }
  for (i = 0; text[i] != '\0'; i++) {
    size_t j;
    for (j = 0; j < needle_n; j++) {
      unsigned char ch = (unsigned char)text[i + j];
      if (ch == '\0') {
        return false;
      }
      if (tolower(ch) != tolower((unsigned char)needle[j])) {
        break;
      }
    }
    if (j == needle_n) {
      return true;
    }
  }
  return false;
}

int BbcMachineParseFilingSystem(const char* text) {
  if (text == NULL) {
    return -1;
  }
  if (EqualsIgnoreCase(text, "dfs") || EqualsIgnoreCase(text, "disc") ||
      EqualsIgnoreCase(text, "disk")) {
    return BBC_FS_DFS;
  }
  if (EqualsIgnoreCase(text, "adfs")) {
    return BBC_FS_ADFS;
  }
  return -1;
}

int BbcMachineRomFilingKind(const char* name) {
  const char* base = name;
  const char* slash;
  if (name == NULL) {
    return BBC_FS_ANY;
  }
  slash = strrchr(name, '/');
  if (slash != NULL && slash[1] != '\0') {
    base = slash + 1;
  }
  if (ContainsIgnoreCase(base, "adfs")) {
    return BBC_FS_ADFS;
  }
  if (ContainsIgnoreCase(base, "dnfs") || ContainsIgnoreCase(base, "dfs")) {
    return BBC_FS_DFS;
  }
  return BBC_FS_ANY;
}

static void NoteRomFiling(BbcMachine* bbc, int slot, const char* name) {
  int kind = BBC_FS_ANY;
  int controller = 0;
  bool wd = false;
  if (slot < 0 || slot > 15) {
    return;
  }
  if (name != NULL) {
    kind = BbcMachineRomFilingKind(name);
  }
  if (kind == BBC_FS_ANY && bbc->sideways[slot] != NULL) {
    if (RomHas(bbc->sideways[slot], "ADFS")) {
      kind = BBC_FS_ADFS;
    } else if (RomHas(bbc->sideways[slot], "DNFS") || RomHas(bbc->sideways[slot], "DFS")) {
      kind = BBC_FS_DFS;
    }
  }
  if (kind == BBC_FS_ADFS) {
    controller = BBC_FDC_1770;
  } else if (kind == BBC_FS_DFS) {
    wd = (name != NULL && ContainsIgnoreCase(name, "1770")) ||
         (bbc->sideways[slot] != NULL && RomHas(bbc->sideways[slot], "1770"));
    controller = wd ? BBC_FDC_1770 : BBC_FDC_8271;
  }
  bbc->rom_fs[slot] = (uint8_t)kind;
  bbc->rom_fdc[slot] = (uint8_t)controller;
}

int BbcMachineControllerForFiling(const BbcMachine* bbc, int filing) {
  int i;
  if (bbc == NULL) {
    return 0;
  }
  if (bbc->master || filing == BBC_FS_ADFS) {
    return BBC_FDC_1770;
  }
  if (filing != BBC_FS_DFS) {
    return 0;
  }
  for (i = 0; i < 16; i++) {
    if (bbc->sideways_loaded[i] && RomHas(bbc->sideways[i], "1770")) {
      return BBC_FDC_1770;
    }
  }
  return BBC_FDC_8271;
}

static void FreeDeferredRomPaths(char** paths, int count) {
  int i;
  for (i = 0; i < count; i++) {
    free(paths[i]);
  }
}

int BbcMachineListRomDirectory(const char* dir, BbcRomFile* files, int capacity, int filing) {
  DIR* handle;
  struct dirent* entry;
  int count = 0;
  bool saw_os = false;
  bool saw_selected = false;
  bool saw_slot[16];
  uint8_t saw_virt[16];
  int slot_kind[16];
  char* deferred_path[8];
  int deferred_kind[8];
  int deferred_named[8];
  int deferred = 0;
  int i;
  if (dir == NULL || files == NULL || capacity < 1 ||
      (filing != BBC_FS_ANY && filing != BBC_FS_DFS && filing != BBC_FS_ADFS)) {
    return -1;
  }
  for (i = 0; i < 16; i++) {
    saw_slot[i] = false;
    saw_virt[i] = 0;
    slot_kind[i] = BBC_FS_ANY;
  }
  handle = opendir(dir);
  if (handle == NULL) {
    fprintf(stderr, "Unable to open ROM directory '%s'\n", dir);
    return -1;
  }
  while ((entry = readdir(handle)) != NULL) {
    int slot = 0;
    int virt = 0;
    int kind;
    char* path;
    struct stat st;
    kind = ClassifyRomName(entry->d_name, &slot, &virt);
    if (kind == 0) {
      continue;
    }
    path = JoinPath(dir, entry->d_name);
    if (path == NULL) {
      closedir(handle);
      FreeDeferredRomPaths(deferred_path, deferred);
      BbcRomFileFree(files, count);
      return -1;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
      free(path);
      if (kind < 0) {
        fprintf(stderr,
                "ROM file '%s' is not os[-name].rom, <socket>[-name].rom, or "
                "<socket>.<virtual>-<name>.rom\n",
                entry->d_name);
        closedir(handle);
        FreeDeferredRomPaths(deferred_path, deferred);
        BbcRomFileFree(files, count);
        return -1;
      }
      continue;
    }
    if (kind < 0) {
      fprintf(stderr,
              "ROM file '%s' is not os[-name].rom, <socket>[-name].rom, or "
              "<socket>.<virtual>-<name>.rom\n",
              entry->d_name);
      free(path);
      closedir(handle);
      FreeDeferredRomPaths(deferred_path, deferred);
      BbcRomFileFree(files, count);
      return -1;
    }
    {
      int rom_kind = slot < 0 ? BBC_FS_ANY : BbcMachineRomFilingKind(entry->d_name);
      if (filing != BBC_FS_ANY && rom_kind != BBC_FS_ANY && rom_kind != filing) {
        free(path);
        continue;
      }
      if (rom_kind == filing) {
        saw_selected = true;
      }
      if (slot >= 0 && virt > 0) {
        if ((saw_virt[slot] & (uint8_t)(1u << virt)) != 0) {
          fprintf(stderr, "ROM directory '%s' has two images for socket %d virtual %d\n", dir,
                  slot, virt);
          free(path);
          closedir(handle);
          FreeDeferredRomPaths(deferred_path, deferred);
          BbcRomFileFree(files, count);
          return -1;
        }
        saw_virt[slot] = (uint8_t)(saw_virt[slot] | (uint8_t)(1u << virt));
      } else if (slot >= 0 && saw_slot[slot]) {
        bool either_fs = (slot_kind[slot] == BBC_FS_DFS && rom_kind == BBC_FS_ADFS) ||
                         (slot_kind[slot] == BBC_FS_ADFS && rom_kind == BBC_FS_DFS);
        if (either_fs) {
          // Hold it until every named socket is known, then fit it in the
          // highest socket nobody else claimed.
          if (deferred == 8) {
            fprintf(stderr, "ROM directory '%s' has no free socket for %s\n", dir, entry->d_name);
            free(path);
            closedir(handle);
            FreeDeferredRomPaths(deferred_path, deferred);
            BbcRomFileFree(files, count);
            return -1;
          }
          deferred_path[deferred] = path;
          deferred_kind[deferred] = rom_kind;
          deferred_named[deferred] = slot;
          deferred++;
          continue;
        } else {
          fprintf(stderr, "ROM directory '%s' has two images for socket %d\n", dir, slot);
          free(path);
          closedir(handle);
          FreeDeferredRomPaths(deferred_path, deferred);
          BbcRomFileFree(files, count);
          return -1;
        }
      } else if (slot >= 0) {
        slot_kind[slot] = rom_kind;
      }
    }
    if (slot < 0) {
      if (saw_os) {
        fprintf(stderr, "ROM directory '%s' has more than one OS image\n", dir);
        free(path);
        closedir(handle);
        FreeDeferredRomPaths(deferred_path, deferred);
        BbcRomFileFree(files, count);
        return -1;
      }
      saw_os = true;
    } else if (virt == 0) {
      saw_slot[slot] = true;
    }
    if (count >= capacity) {
      fprintf(stderr, "ROM directory '%s' has too many images\n", dir);
      free(path);
      closedir(handle);
      FreeDeferredRomPaths(deferred_path, deferred);
      BbcRomFileFree(files, count);
      return -1;
    }
    files[count].slot = slot;
    files[count].virt = virt;
    files[count].path = path;
    count++;
  }
  closedir(handle);
  for (i = 0; i < deferred; i++) {
    int alt = -1;
    int socket;
    for (socket = 15; socket >= 0; socket--) {
      if (!saw_slot[socket]) {
        alt = socket;
        break;
      }
    }
    if (alt < 0 || count >= capacity) {
      fprintf(stderr, "ROM directory '%s' has no free socket for a second filing system\n", dir);
      for (; i < deferred; i++) {
        free(deferred_path[i]);
      }
      BbcRomFileFree(files, count);
      return -1;
    }
    fprintf(stderr, "ROM %s shares socket %d with the other filing system; fitted in socket %d\n",
            strrchr(deferred_path[i], '/') != NULL ? strrchr(deferred_path[i], '/') + 1 : deferred_path[i],
            deferred_named[i], alt);
    saw_slot[alt] = true;
    slot_kind[alt] = deferred_kind[i];
    files[count].slot = alt;
    files[count].virt = 0;
    files[count].path = deferred_path[i];
    count++;
  }
  if (filing != BBC_FS_ANY && !saw_selected) {
    fprintf(stderr, "ROM directory '%s' has no %s image\n", dir, filing == BBC_FS_DFS ? "DFS" : "ADFS");
    BbcRomFileFree(files, count);
    return -1;
  }
  return count;
}

const char* BbcRomOsPath(const BbcRomFile* files, int count) {
  int i;
  if (files == NULL) {
    return NULL;
  }
  for (i = 0; i < count; i++) {
    if (files[i].slot < 0) {
      return files[i].path;
    }
  }
  return NULL;
}

bool BbcMachineLoadRomFiles(BbcMachine* bbc, const BbcRomFile* files, int count,
                            const bool skip_slot[16]) {
  int i;
  if (bbc == NULL) {
    return false;
  }
  for (i = 0; i < count; i++) {
    int slot = files[i].slot;
    if (slot < 0) {
      continue;
    }
    if (skip_slot != NULL && skip_slot[slot]) {
      continue;
    }
    if (files[i].virt > 0) {
      fprintf(stderr, "ROM %d.%d: %s\n", slot, files[i].virt, files[i].path);
      if (!LoadSidewaysVirtual(bbc, slot, files[i].virt, files[i].path)) {
        return false;
      }
      continue;
    }
    fprintf(stderr, "ROM %d: %s\n", slot, files[i].path);
    if (!BbcMachineLoadSideways(bbc, slot, files[i].path)) {
      return false;
    }
  }
  return true;
}

void BbcMachineSetRam(BbcMachine* bbc, uint8_t* ram) {
  bbc->ram = ram;
  MapRomsel(bbc);
}

BbcMachine* BbcMachineCreate(void) {
  BbcMachine* bbc = calloc(1, sizeof(*bbc));
  int i;
  if (bbc == NULL) {
    return NULL;
  }
  bbc->jim = malloc(65536);
  bbc->fb = malloc((size_t)BBC_FB_WIDTH * BBC_FB_HEIGHT * 3);
  bbc->beam_fb = malloc((size_t)BBC_FB_WIDTH * BBC_FB_HEIGHT * 3);
  if (bbc->jim == NULL || bbc->fb == NULL || bbc->beam_fb == NULL) {
    free(bbc->jim);
    free(bbc->fb);
    free(bbc->beam_fb);
    free(bbc);
    return NULL;
  }
  memset(bbc->fred, 0xff, sizeof(bbc->fred));
  memset(bbc->jim, 0xff, 65536);
  memset(bbc->fb, 0, (size_t)BBC_FB_WIDTH * BBC_FB_HEIGHT * 3);
  memset(bbc->beam_fb, 0, (size_t)BBC_FB_WIDTH * BBC_FB_HEIGHT * 3);
  bbc->jim_page = 0;
  bbc->fred[0xff] = 0;
  bbc->mapped_slot = -1;
  bbc->mapped_virt = -1;
  bbc->model = BBC_MACHINE_B;
  ViaReset(&bbc->sys);
  ViaReset(&bbc->user);
  // Bits 6 and 7 are the caps and shift LEDs, active low. The other latch
  // bits stay clear so the keyboard is enabled and the screen is unshifted.
  bbc->ic32 = 0xc0;
  bbc->tube_status = 0xfe;
  bbc->acia_tx_empty = true;
  bbc->acia_baud_tx = 19200;
  bbc->acia_baud_rx = 19200;
  bbc->fdc.kind = BBC_FDC_8271;
  bbc->fdc.command = 0xff;
  bbc->fdc.direction = 1;
  bbc->noise_lfsr = 0x8000;
  bbc->do_even_frame = true;
  bbc->first_scanline = true;
  bbc->tt_fg = 7;
  for (i = 0; i < 4; i++) {
    bbc->tone_vol[i] = 15;
    bbc->analogue[i] = 0x8000;
  }
  for (i = 0; i < 16; i++) {
    bbc->ula_pal[i] = 7;
  }
  if (pthread_mutex_init(&bbc->audio_mu, NULL) == 0) {
    bbc->audio_ready = true;
  }
  bbc->volume = 7;
  bbc->disc_listen_fd = -1;
  bbc->disc_handshake_fd = -1;
  bbc->econet.fd = -1;
  bbc->tape_fd = -1;
  bbc->tape_listen_fd = -1;
  bbc->tape_handshake_fd = -1;
  for (i = 0; i < 2; i++) {
    bbc->fdc.disc[i].fd = -1;
    bbc->fdc.extra[i].fd = -1;
  }
  return bbc;
}

void BbcMachineSetTube(BbcMachine* bbc, const BbcTubeHooks* hooks) {
  void (*destroy)(void*);
  void* parasite;
  if (bbc == NULL) {
    return;
  }
  destroy = bbc->parasite_destroy;
  parasite = bbc->parasite;
  bbc->parasite_destroy = NULL;
  bbc->parasite = NULL;
  bbc->parasite_run = NULL;
  bbc->parasite_reset = NULL;
  bbc->parasite_read = NULL;
  bbc->parasite_pc = NULL;
  bbc->tube = NULL;
  if (destroy != NULL) {
    destroy(parasite);
  }
  if (hooks == NULL) {
    return;
  }
  bbc->tube = hooks->tube;
  bbc->parasite = hooks->parasite;
  bbc->parasite_run = hooks->run;
  bbc->parasite_reset = hooks->reset;
  bbc->parasite_destroy = hooks->destroy;
  bbc->parasite_read = hooks->read;
  bbc->parasite_pc = hooks->pc;
}

void BbcMachineRunParasite(BbcMachine* bbc, int host_cycles) {
  if (bbc != NULL && bbc->parasite_run != NULL && host_cycles > 0) {
    bbc->parasite_run(bbc->parasite, host_cycles);
  }
}

uint8_t BbcMachineParasiteRead(const BbcMachine* bbc, uint16_t addr) {
  if (bbc == NULL || bbc->parasite_read == NULL) {
    return 0;
  }
  return bbc->parasite_read(bbc->parasite, addr);
}

uint16_t BbcMachineParasitePc(const BbcMachine* bbc) {
  if (bbc == NULL || bbc->parasite_pc == NULL) {
    return 0;
  }
  return bbc->parasite_pc(bbc->parasite);
}

void BbcMachineDestroy(BbcMachine* bbc) {
  int i;
  if (bbc == NULL) {
    return;
  }
  BbcMachineSetTube(bbc, NULL);
  for (i = 0; i < 16; i++) {
    int page;
    free(bbc->sideways[i]);
    for (page = 0; page < 7; page++) {
      free(bbc->sideways_page[i][page]);
    }
  }
  TapeFlush(bbc);
  TapeClose(bbc);
  TapeStop(bbc);
  EconetClose(bbc);
  DiscStop(bbc);
  HdRelease(bbc);
  for (i = 0; i < 2; i++) {
    DiscRelease(&bbc->fdc.disc[i]);
    DiscRelease(&bbc->fdc.extra[i]);
  }
  if (bbc->audio_ready) {
    pthread_mutex_destroy(&bbc->audio_mu);
  }
  free(bbc->tape);
  free(bbc->tape_path);
  free(bbc->printed);
  free(bbc->printer_path);
  free(bbc->lynne);
  free(bbc->hazel);
  free(bbc->andy);
  free(bbc->mos_low);
  free(bbc->os_rom);
  free(bbc->jim);
  free(bbc->fb);
  free(bbc->beam_fb);
  free(bbc);
}

void BbcMachineSetAnalogue(BbcMachine* bbc, int channel, int value) {
  if (bbc == NULL || channel < 0 || channel > 3) {
    return;
  }
  if (value < 0) {
    value = 0;
  }
  if (value > 65535) {
    value = 65535;
  }
  bbc->analogue[channel] = (uint16_t)value;
}

void BbcMachineSetFire(BbcMachine* bbc, int button, bool down) {
  if (bbc == NULL || button < 0 || button > 1) {
    return;
  }
  bbc->fire[button] = down;
}

int BbcMachineParseKeyboard(const char* text) {
  if (text != NULL && strcmp(text, "uk") == 0) {
    return BBC_KEYBOARD_UK;
  }
  if (text != NULL && strcmp(text, "us") == 0) {
    return BBC_KEYBOARD_US;
  }
  return -1;
}

void BbcMachineSetKeyboard(BbcMachine* bbc, int kind) {
  if (bbc == NULL) {
    return;
  }
  bbc->keyboard = kind == BBC_KEYBOARD_US ? BBC_KEYBOARD_US : BBC_KEYBOARD_UK;
}

int BbcMachineKeyboard(const BbcMachine* bbc) {
  return bbc == NULL ? BBC_KEYBOARD_UK : bbc->keyboard;
}

bool BbcMachineCapsLed(const BbcMachine* bbc) {
  return bbc != NULL && (bbc->ic32 & 0x40) == 0;
}

bool BbcMachineShiftLed(const BbcMachine* bbc) {
  return bbc != NULL && (bbc->ic32 & 0x80) == 0;
}

bool BbcMachineMotorOn(const BbcMachine* bbc) {
  return bbc != NULL && MotorOn(bbc);
}

size_t BbcMachinePrinterLength(const BbcMachine* bbc) {
  return bbc == NULL ? 0 : bbc->printed_len;
}

uint8_t BbcMachinePrinterByte(const BbcMachine* bbc, size_t index) {
  if (bbc == NULL || index >= bbc->printed_len) {
    return 0;
  }
  return bbc->printed[index];
}

void BbcMachineSetPrinter(BbcMachine* bbc, const char* path) {
  if (bbc == NULL) {
    return;
  }
  free(bbc->printer_path);
  bbc->printer_path = NULL;
  if (path != NULL) {
    bbc->printer_path = strdup(path);
  }
}

static void TapeReset(BbcMachine* bbc) {
  free(bbc->tape);
  bbc->tape = NULL;
  bbc->tape_len = 0;
  bbc->tape_pos = 0;
  bbc->tape_dirty = false;
}

static void TapeIngest(BbcMachine* bbc, const uint8_t* buf, size_t length) {
  size_t i;
  TapeReset(bbc);
  if (length >= 12 && buf != NULL && memcmp(buf, "UEF File!", 9) == 0) {
    size_t cursor = 12;
    while (cursor + 6 <= length) {
      uint16_t id = (uint16_t)(buf[cursor] | (buf[cursor + 1] << 8));
      uint32_t chunk = (uint32_t)buf[cursor + 2] | ((uint32_t)buf[cursor + 3] << 8) |
                       ((uint32_t)buf[cursor + 4] << 16) | ((uint32_t)buf[cursor + 5] << 24);
      cursor += 6;
      if (cursor + chunk > length) {
        break;
      }
      if (id == 0x0110 && chunk >= 2) {
        int cycles = buf[cursor] | (buf[cursor + 1] << 8);
        int count = cycles / 20;
        int n;
        if (count < 1) {
          count = 1;
        }
        for (n = 0; n < count; n++) {
          TapeAppend(bbc, 0xaa, 1);
        }
      } else if (id == 0x0100 || id == 0x0104 || id == 0x0102) {
        uint32_t n;
        for (n = 0; n < chunk; n++) {
          TapeAppend(bbc, buf[cursor + n], 0);
        }
      }
      cursor += chunk;
    }
  } else if (length > 0 && buf != NULL) {
    for (i = 0; i < 32; i++) {
      TapeAppend(bbc, 0xaa, 1);
    }
    for (i = 0; i < length; i++) {
      TapeAppend(bbc, buf[i], 0);
    }
  }
  bbc->tape_pos = 0;
  bbc->tape_dirty = false;
}

static int TapePut(uint8_t** buf, size_t* len, size_t* cap, const void* bytes, size_t n) {
  uint8_t* grown;
  size_t next;
  if (n == 0) {
    return 0;
  }
  if (*len > CASSETTE_MAX_IMAGE || n > CASSETTE_MAX_IMAGE - *len) {
    return -1;
  }
  next = *cap == 0 ? 256 : *cap;
  while (next < *len + n) {
    if (next > CASSETTE_MAX_IMAGE / 2) {
      next = CASSETTE_MAX_IMAGE;
      break;
    }
    next *= 2;
  }
  if (next < *len + n) {
    return -1;
  }
  if (next != *cap) {
    grown = realloc(*buf, next);
    if (grown == NULL) {
      return -1;
    }
    *buf = grown;
    *cap = next;
  }
  memcpy(*buf + *len, bytes, n);
  *len += n;
  return 0;
}

static uint8_t* TapeEncode(const BbcMachine* bbc, size_t* out_len) {
  uint8_t header[12] = {'U', 'E', 'F', ' ', 'F', 'i', 'l', 'e', '!', 0, 0x0a, 0};
  uint8_t* buf = NULL;
  size_t len = 0;
  size_t cap = 0;
  size_t i = 0;
  *out_len = 0;
  if (TapePut(&buf, &len, &cap, header, sizeof(header)) != 0) {
    free(buf);
    return NULL;
  }
  while (i < bbc->tape_len) {
    int carrier = bbc->tape[i].dcd != 0;
    size_t start = i;
    size_t count;
    uint8_t chunk[6];
    while (i < bbc->tape_len && (bbc->tape[i].dcd != 0) == carrier) {
      i++;
    }
    count = i - start;
    if (carrier) {
      while (count > 0) {
        size_t piece = count > 3000 ? 3000 : count;
        uint32_t cycles = (uint32_t)piece * 20u;
        chunk[0] = 0x10;
        chunk[1] = 0x01;
        chunk[2] = 2;
        chunk[3] = 0;
        chunk[4] = 0;
        chunk[5] = 0;
        if (TapePut(&buf, &len, &cap, chunk, sizeof(chunk)) != 0) {
          free(buf);
          return NULL;
        }
        chunk[0] = (uint8_t)cycles;
        chunk[1] = (uint8_t)(cycles >> 8);
        if (TapePut(&buf, &len, &cap, chunk, 2) != 0) {
          free(buf);
          return NULL;
        }
        count -= piece;
      }
    } else {
      chunk[0] = 0x00;
      chunk[1] = 0x01;
      chunk[2] = (uint8_t)count;
      chunk[3] = (uint8_t)(count >> 8);
      chunk[4] = (uint8_t)(count >> 16);
      chunk[5] = (uint8_t)(count >> 24);
      if (TapePut(&buf, &len, &cap, chunk, sizeof(chunk)) != 0) {
        free(buf);
        return NULL;
      }
      for (count = 0; count < i - start; count++) {
        if (TapePut(&buf, &len, &cap, &bbc->tape[start + count].data, 1) != 0) {
          free(buf);
          return NULL;
        }
      }
    }
  }
  *out_len = len;
  return buf;
}

static void TapeClose(BbcMachine* bbc) {
  if (bbc->tape_fd >= 0) {
    shutdown(bbc->tape_fd, SHUT_RDWR);
    SocketClose(bbc->tape_fd);
    bbc->tape_fd = -1;
  }
}

static int TapeWriteFile(const char* path, const uint8_t* data, size_t length) {
  FILE* fp = fopen(path, "wb");
  if (fp == NULL) {
    return -1;
  }
  if (length > 0 && fwrite(data, 1, length, fp) != length) {
    fclose(fp);
    return -1;
  }
  if (fflush(fp) != 0) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

static void TapeFlush(BbcMachine* bbc) {
  uint8_t* encoded;
  size_t length = 0;
  if (bbc == NULL || !bbc->tape_dirty || bbc->tape_protect) {
    return;
  }
  if (bbc->tape_fd < 0 && bbc->tape_path == NULL) {
    return;
  }
  encoded = TapeEncode(bbc, &length);
  if (encoded == NULL) {
    return;
  }
  if (bbc->tape_fd >= 0) {
    if (CassetteSendImage(bbc->tape_fd, encoded, length) != 0) {
      TapeClose(bbc);
    } else {
      bbc->tape_dirty = false;
    }
  } else if (TapeWriteFile(bbc->tape_path, encoded, length) == 0) {
    bbc->tape_dirty = false;
  }
  free(encoded);
}

bool BbcMachineLoadTape(BbcMachine* bbc, const char* path) {
  uint8_t* buf = NULL;
  size_t length = 0;
  char* copy;
  if (bbc == NULL || path == NULL || !ReadWholeFile(path, &buf, &length)) {
    return false;
  }
  copy = strdup(path);
  TapeIngest(bbc, buf, length);
  free(buf);
  free(bbc->tape_path);
  bbc->tape_path = copy;
  bbc->tape_protect = false;
  return true;
}

struct TapePull {
  BbcMachine* bbc;
  int fd;
};

static int TapePullRead(void* ctx, void* buf, size_t n) {
  struct TapePull* pull = (struct TapePull*)ctx;
  uint8_t* bytes = (uint8_t*)buf;
  size_t off = 0;
  while (off < n) {
    struct pollfd pfd;
    int ready;
    bool stop;
    ssize_t got;
    pthread_mutex_lock(&pull->bbc->tape_mu);
    stop = pull->bbc->tape_listen_stop;
    pthread_mutex_unlock(&pull->bbc->tape_mu);
    if (stop) {
      return -1;
    }
    pfd.fd = pull->fd;
    pfd.events = POLLIN;
    ready = SocketPoll(&pfd, 1, 200);
    if (ready < 0) {
      if (SocketInterrupted()) {
        continue;
      }
      return -1;
    }
    if (ready == 0 || (pfd.revents & (POLLIN | POLLHUP)) == 0) {
      if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
        return -1;
      }
      continue;
    }
    got = SocketRead(pull->fd, bytes + off, n - off);
    if (got < 0) {
      if (SocketInterrupted()) {
        continue;
      }
      return -1;
    }
    if (got == 0) {
      return -1;
    }
    off += (size_t)got;
  }
  return 0;
}

static void TapeEnqueue(BbcMachine* bbc, int fd, CassetteInsert* insert) {
  struct TapePending* item = calloc(1, sizeof(*item));
  struct TapePending* tail;
  if (item == NULL) {
    CassetteSendReply(fd, CASSETTE_ERR, "out of memory");
    CassetteInsertFree(insert);
    SocketClose(fd);
    return;
  }
  item->fd = fd;
  item->insert = *insert;
  memset(insert, 0, sizeof(*insert));
  pthread_mutex_lock(&bbc->tape_mu);
  tail = bbc->tape_pending;
  if (tail == NULL) {
    bbc->tape_pending = item;
  } else {
    while (tail->next != NULL) {
      tail = tail->next;
    }
    tail->next = item;
  }
  __atomic_store_n(&bbc->tape_attention, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&bbc->tape_mu);
}

static void* TapeListenMain(void* arg) {
  BbcMachine* bbc = (BbcMachine*)arg;
  for (;;) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd;
    bool stop;
    struct pollfd listen_pfd;
    int ready;
    pthread_mutex_lock(&bbc->tape_mu);
    stop = bbc->tape_listen_stop;
    pthread_mutex_unlock(&bbc->tape_mu);
    if (stop) {
      break;
    }
    listen_pfd.fd = bbc->tape_listen_fd;
    listen_pfd.events = POLLIN;
    ready = SocketPoll(&listen_pfd, 1, 200);
    if (ready < 0) {
      if (SocketInterrupted()) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      continue;
    }
    fd = (int)accept(bbc->tape_listen_fd, (struct sockaddr*)&addr, &len);
    if (fd < 0) {
      if (SocketWouldBlock()) {
        continue;
      }
      break;
    }
    {
      int one = 1;
#ifdef SO_NOSIGPIPE
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    }
    pthread_mutex_lock(&bbc->tape_mu);
    bbc->tape_handshake_fd = fd;
    stop = bbc->tape_listen_stop;
    pthread_mutex_unlock(&bbc->tape_mu);
    if (stop) {
      SocketClose(fd);
      break;
    }
    {
      struct TapePull pull;
      CassetteInsert insert;
      pull.bbc = bbc;
      pull.fd = fd;
      if (CassetteReadInsert(TapePullRead, &pull, &insert) != 0) {
        SocketClose(fd);
      } else {
        TapeEnqueue(bbc, fd, &insert);
      }
    }
    pthread_mutex_lock(&bbc->tape_mu);
    if (bbc->tape_handshake_fd == fd) {
      bbc->tape_handshake_fd = -1;
    }
    pthread_mutex_unlock(&bbc->tape_mu);
  }
  return NULL;
}

static void TapeWatch(BbcMachine* bbc) {
  struct pollfd pfd;
  char drain[32];
  ssize_t n;
  int i;
  if (bbc->tape_fd < 0) {
    return;
  }
  pfd.fd = bbc->tape_fd;
  pfd.events = POLLIN;
  if (SocketPoll(&pfd, 1, 0) <= 0) {
    return;
  }
  if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
    return;
  }
  n = SocketRead(bbc->tape_fd, drain, sizeof(drain));
  if (n == 0 || (n < 0 && !SocketWouldBlock())) {
    TapeClose(bbc);
    TapeReset(bbc);
    bbc->tape_protect = false;
    return;
  }
  for (i = 0; i < n; i++) {
    if (drain[i] == CASSETTE_REWIND) {
      bbc->tape_pos = 0;
    }
  }
}

static void TapeService(BbcMachine* bbc) {
  struct TapePending* pending;
  if (bbc == NULL || !bbc->tape_listen_started) {
    return;
  }
  TapeWatch(bbc);
  pthread_mutex_lock(&bbc->tape_mu);
  pending = bbc->tape_pending;
  bbc->tape_pending = NULL;
  pthread_mutex_unlock(&bbc->tape_mu);
  while (pending != NULL) {
    struct TapePending* next = pending->next;
    const char* name = pending->insert.name != NULL ? pending->insert.name : "tape";
    if (bbc->tape_dirty) {
      TapeFlush(bbc);
    }
    TapeClose(bbc);
    free(bbc->tape_path);
    bbc->tape_path = NULL;
    TapeIngest(bbc, pending->insert.data, pending->insert.length);
    bbc->tape_fd = pending->fd;
    bbc->tape_protect = pending->insert.protect != 0;
    pending->fd = -1;
    if (CassetteSendReply(bbc->tape_fd, CASSETTE_OK, "inserted") != 0) {
      TapeClose(bbc);
      TapeReset(bbc);
    } else {
      fprintf(stderr, "Tape: %s\n", name);
    }
    CassetteInsertFree(&pending->insert);
    free(pending);
    pending = next;
  }
}

static void TapeStop(BbcMachine* bbc) {
  struct TapePending* pending;
  int handshake;
  if (bbc == NULL || !bbc->tape_listen_started) {
    return;
  }
  pthread_mutex_lock(&bbc->tape_mu);
  bbc->tape_listen_stop = true;
  handshake = bbc->tape_handshake_fd;
  pthread_mutex_unlock(&bbc->tape_mu);
  if (bbc->tape_listen_fd >= 0) {
    shutdown(bbc->tape_listen_fd, SHUT_RDWR);
  }
  if (handshake >= 0) {
    shutdown(handshake, SHUT_RDWR);
  }
  pthread_join(bbc->tape_listen_thread, NULL);
  if (bbc->tape_listen_fd >= 0) {
    SocketClose(bbc->tape_listen_fd);
    bbc->tape_listen_fd = -1;
  }
  pthread_mutex_lock(&bbc->tape_mu);
  pending = bbc->tape_pending;
  bbc->tape_pending = NULL;
  pthread_mutex_unlock(&bbc->tape_mu);
  while (pending != NULL) {
    struct TapePending* next = pending->next;
    if (pending->fd >= 0) {
      SocketClose(pending->fd);
    }
    CassetteInsertFree(&pending->insert);
    free(pending);
    pending = next;
  }
  pthread_mutex_destroy(&bbc->tape_mu);
  bbc->tape_listen_started = false;
}

int BbcMachineListenTapes(BbcMachine* bbc, int port) {
  struct sockaddr_in addr;
  socklen_t len;
  int fd;
  int one = 1;
  if (bbc == NULL || bbc->tape_listen_started || port < 0 || port > 65535) {
    return -1;
  }
  if (!SocketStartup()) {
    return -1;
  }
  fd = (int)socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
#ifdef _WIN32
  setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
#else
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
    SocketClose(fd);
    return -1;
  }
  len = sizeof(addr);
  if (getsockname(fd, (struct sockaddr*)&addr, &len) != 0) {
    SocketClose(fd);
    return -1;
  }
  if (pthread_mutex_init(&bbc->tape_mu, NULL) != 0) {
    SocketClose(fd);
    return -1;
  }
  bbc->tape_listen_fd = fd;
  bbc->tape_listen_port = ntohs(addr.sin_port);
  bbc->tape_handshake_fd = -1;
  bbc->tape_listen_stop = false;
  bbc->tape_pending = NULL;
  bbc->tape_listen_started = true;
  if (pthread_create(&bbc->tape_listen_thread, NULL, TapeListenMain, bbc) != 0) {
    bbc->tape_listen_started = false;
    pthread_mutex_destroy(&bbc->tape_mu);
    SocketClose(fd);
    bbc->tape_listen_fd = -1;
    return -1;
  }
  return bbc->tape_listen_port;
}

void BbcMachineSetKey(BbcMachine* bbc, int column, int row, bool down) {
  if (bbc == NULL || column < 0 || column > 15 || row < 0 || row > 7) {
    return;
  }
  bbc->key_down[column][row] = down ? 1 : 0;
  UpdateKeyboard(bbc);
}

void BbcMachineSetVolume(BbcMachine* bbc, int volume) {
  if (bbc == NULL) {
    return;
  }
  if (volume < 0) {
    volume = 0;
  }
  if (volume > 11) {
    volume = 11;
  }
  bbc->volume = volume;
}

int BbcMachineReadAudio(BbcMachine* bbc, int16_t* dst, int max_samples) {
  int count = 0;
  int volume;
  if (bbc == NULL || dst == NULL || max_samples <= 0 || !bbc->audio_ready) {
    return 0;
  }
  volume = bbc->volume;
  pthread_mutex_lock(&bbc->audio_mu);
  while (count < max_samples && bbc->audio_r != bbc->audio_w) {
    int32_t sample = bbc->audio_ring[bbc->audio_r];
    bbc->audio_r = (bbc->audio_r + 1) % BBC_AUDIO_RING;
    if (volume <= 0) {
      sample = 0;
    } else if (volume < 11) {
      sample = sample * volume / 11;
    }
    dst[count++] = (int16_t)sample;
  }
  pthread_mutex_unlock(&bbc->audio_mu);
  return count;
}

typedef struct {
  Device device;
  BbcMachine* bbc;
} BbcIo;

static bool BbcClaim(Device* dev, uint16_t addr) {
  (void)dev;
  return addr >= 0xfc00 && addr <= 0xfeff;
}

static void BbcWriteDev(Device* dev, uint16_t addr, int value) {
  BbcMachineWrite(((BbcIo*)dev)->bbc, addr, (uint8_t)value);
}

static int BbcReadDev(Device* dev, uint16_t addr) {
  return BbcMachineRead(((BbcIo*)dev)->bbc, addr);
}

Device* BbcIoDeviceCreate(BbcMachine* bbc) {
  BbcIo* io = calloc(1, sizeof(*io));
  if (io == NULL) {
    return NULL;
  }
  io->bbc = bbc;
  io->device.claim = BbcClaim;
  io->device.write = BbcWriteDev;
  io->device.read = BbcReadDev;
  return &io->device;
}
