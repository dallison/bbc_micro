//
//  bbc_hardware.c
//  BBC Micro Model B: SHEILA, FRED, JIM, the video ULA, and a framebuffer.
//

#include "bbc_hardware.h"
#include "bbc_font.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
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
  pthread_mutex_t audio_mu;
  bool audio_ready;
  uint8_t adc_status;
  uint16_t adc[4];
  uint8_t tube_status;
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
    bool nmi_edge;
    bool writing;
    uint8_t buffer[1024];
    uint8_t track_image[8192];
    int track_left;
    struct {
      uint8_t* data;
      size_t length;
      int tracks;
      int sides;
      int spt;
      bool protect;
      char* path;
    } disc[2];
  } fdc;
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
  bool sideways_loaded[16];
  bool sideways_ram[16];
  bool any_sideways;
  int model;
  bool master;
  int mapped_slot;
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

static void InvertRect(BbcMachine* bbc, int x, int y, int w, int h) {
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
  InvertRect(bbc, x, y + start, w, end - start + 1);
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
      int y = row * scanlines;
      bool control = byte < 0x20;
      uint8_t glyph = byte;
      bool graphic = bbc->tt_graphics && !control;
      bool hidden = bbc->tt_conceal || (bbc->tt_flash && !FlashOn(bbc));
      int fg = bbc->tt_fg & 7;
      int bg = bbc->tt_bg & 7;
      if (control && bbc->tt_hold && bbc->tt_graphics && bbc->tt_held_valid) {
        glyph = bbc->tt_held;
        graphic = true;
        control = false;
      }
      if (control || hidden || glyph < 0x20) {
        FillCell(bbc, x, y, ppc, scanlines, kRgb[bg]);
      } else if (graphic) {
        static const uint8_t kBit[6] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x40};
        int sy;
        int vspan = bbc->tt_double ? scanlines * 2 : scanlines;
        int origin = bbc->tt_double && bbc->tt_bottom ? scanlines : 0;
        for (sy = 0; sy < scanlines; sy++) {
          int band = ((origin + sy) * 3) / (vspan > 0 ? vspan : 1);
          int s;
          if (band > 2) {
            band = 2;
          }
          for (s = 0; s < 2; s++) {
            int which = band * 2 + s;
            const uint8_t* colour = (glyph & kBit[which]) ? kRgb[fg] : kRgb[bg];
            int x0 = x + s * (ppc / 2);
            int w = ppc / 2;
            if (bbc->tt_separated) {
              FillCell(bbc, x0, y + sy, w, 1, kRgb[bg]);
              if (w > 2) {
                FillCell(bbc, x0 + 1, y + sy, w - 2, 1, colour);
              }
            } else {
              FillCell(bbc, x0, y + sy, w, 1, colour);
            }
          }
        }
      } else {
        int rows_used = bbc->tt_double ? 4 : 8;
        int src = bbc->tt_double && bbc->tt_bottom ? 4 : 0;
        int fy;
        FillCell(bbc, x, y, ppc, scanlines, kRgb[bg]);
        for (fy = 0; fy < rows_used; fy++) {
          int y0 = y + fy * scanlines / rows_used;
          int y1 = y + (fy + 1) * scanlines / rows_used;
          uint8_t bits = kBbcFont8x8[glyph][src + fy];
          int fx;
          for (fx = 0; fx < 8; fx++) {
            int x0;
            int x1;
            int yy;
            int xx;
            if ((bits & (1 << fx)) == 0) {
              continue;
            }
            x0 = x + fx * ppc / 8;
            x1 = x + (fx + 1) * ppc / 8;
            for (yy = y0; yy < y1; yy++) {
              for (xx = x0; xx < x1; xx++) {
                PutPixel(bbc, xx, yy, kRgb[fg]);
              }
            }
          }
        }
      }
      if (byte < 0x20) {
        TeletextApply(bbc, byte);
      } else if (bbc->tt_graphics) {
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
  if (rows * scanlines > BBC_FB_HEIGHT) {
    rows = BBC_FB_HEIGHT / scanlines;
  }
  bbc->frame_width = cols * ppc;
  bbc->frame_height = rows * scanlines;
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
  struct pollfd fd;
  uint8_t ch;
  if ((bbc->acia_control & 0x03) == 0x03) {
    return;
  }
  if (bbc->acia_tx_wait > 0) {
    bbc->acia_tx_wait -= cycles;
    if (bbc->acia_tx_wait <= 0) {
      bbc->acia_tx_wait = 0;
      bbc->acia_tx_empty = true;
      if (CassetteSelected(bbc) && MotorOn(bbc) && bbc->tape_pos >= bbc->tape_len) {
        TapeAppend(bbc, bbc->acia_tx, 0);
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
  fd.fd = 0;
  fd.events = POLLIN;
  fd.revents = 0;
  if (poll(&fd, 1, 0) != 1 || read(0, &ch, 1) != 1) {
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
  return !bbc->sideways_ram[bbc->romsel & 0x0f];
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

static void MapRomsel(BbcMachine* bbc) {
  int slot;
  bool andy;
  bool have;
  if (bbc->ram == NULL) {
    return;
  }
  slot = bbc->romsel & 0x0f;
  andy = bbc->master && (bbc->romsel & 0x80) != 0;
  if (!SidewaysActive(bbc)) {
    bbc->mapped_slot = slot;
    bbc->andy_mapped = false;
    return;
  }
  if (slot == bbc->mapped_slot && andy == bbc->andy_mapped) {
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
  have = bbc->sideways[slot] != NULL && (bbc->sideways_loaded[slot] || bbc->sideways_ram[slot]);
  if (have) {
    if (andy) {
      memcpy(bbc->ram + 0x9000, bbc->sideways[slot] + 0x1000, 0x3000);
    } else {
      memcpy(bbc->ram + 0x8000, bbc->sideways[slot], 16384);
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
  MapRomsel(bbc);
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

static void FdcSetLine(BbcMachine* bbc, bool on) {
  if (on && !bbc->fdc.nmi_line) {
    bbc->fdc.nmi_edge = true;
  }
  bbc->fdc.nmi_line = on;
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

static bool FdcNoDisc(const BbcMachine* bbc) {
  int drive = bbc->fdc.drive;
  return drive < 0 || drive > 1 || bbc->fdc.disc[drive].data == NULL;
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
  const int drive = bbc->fdc.drive;
  int track;
  size_t off;
  if (FdcNoDisc(bbc) || size < 1) {
    return -1;
  }
  track = Fdc1770(bbc) ? bbc->fdc.track_reg : bbc->fdc.params[0];
  if (track < 0 || track >= bbc->fdc.disc[drive].tracks || bbc->fdc.side < 0 ||
      bbc->fdc.side >= bbc->fdc.disc[drive].sides || bbc->fdc.sector < 0 ||
      bbc->fdc.sector >= bbc->fdc.disc[drive].spt) {
    return -1;
  }
  off = (((size_t)track * (size_t)bbc->fdc.disc[drive].sides + (size_t)bbc->fdc.side) *
             (size_t)bbc->fdc.disc[drive].spt +
         (size_t)bbc->fdc.sector) *
        256;
  if (off + (size_t)size > bbc->fdc.disc[drive].length) {
    return -1;
  }
  return (int)off;
}

static void DiscFlush(BbcMachine* bbc, int drive) {
  FILE* fp;
  if (drive < 0 || drive > 1 || bbc->fdc.disc[drive].protect || bbc->fdc.disc[drive].path == NULL ||
      bbc->fdc.disc[drive].data == NULL) {
    return;
  }
  fp = fopen(bbc->fdc.disc[drive].path, "r+b");
  if (fp == NULL) {
    return;
  }
  if (fwrite(bbc->fdc.disc[drive].data, 1, bbc->fdc.disc[drive].length, fp) !=
      bbc->fdc.disc[drive].length) {
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
      FdcAsk(bbc, true, bbc->fdc.disc[bbc->fdc.drive].data[bbc->fdc.sector_off]);
    }
    bbc->fdc.sector_off++;
    bbc->fdc.bytes_left--;
    return;
  }
  if (bbc->fdc.phase == FDC_WRITE) {
    if (bbc->fdc.sector_off < 0) {
      int drive = bbc->fdc.drive;
      if (!FdcNoDisc(bbc) && bbc->fdc.disc[drive].protect) {
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
  int drive;
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
  drive = bbc->fdc.drive;
  n = bbc->fdc.buf_i;
  if (!FdcNoDisc(bbc) && bbc->fdc.sector_off >= 0 && n > 0 &&
      (size_t)bbc->fdc.sector_off + (size_t)n <= bbc->fdc.disc[drive].length) {
    memcpy(bbc->fdc.disc[drive].data + bbc->fdc.sector_off, bbc->fdc.buffer, (size_t)n);
    DiscFlush(bbc, drive);
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
  bbc->fdc.nmi_edge = false;
  bbc->fdc.nmi_line = false;
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
      int drive = bbc->fdc.drive;
      uint8_t fill = bbc->fdc.params[4];
      if (FdcNoDisc(bbc)) {
        FdcFinish(bbc, 0x10);
        return;
      }
      if (bbc->fdc.disc[drive].protect) {
        FdcFinish(bbc, 0x12);
        return;
      }
      FdcLatchSide(bbc);
      if (track >= 0 && track < bbc->fdc.disc[drive].tracks &&
          bbc->fdc.side < bbc->fdc.disc[drive].sides) {
        int sector;
        for (sector = 0; sector < bbc->fdc.disc[drive].spt; sector++) {
          size_t off = (((size_t)track * (size_t)bbc->fdc.disc[drive].sides +
                         (size_t)bbc->fdc.side) *
                            (size_t)bbc->fdc.disc[drive].spt +
                        (size_t)sector) *
                       256;
          if (off + 256 <= bbc->fdc.disc[drive].length) {
            memset(bbc->fdc.disc[drive].data + off, fill, 256);
          }
        }
        DiscFlush(bbc, drive);
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
      if (!FdcNoDisc(bbc) && (bbc->fdc.spin % 400000) < 8000) {
        result |= 0x10;
      }
      if (drive >= 0 && drive <= 1 && bbc->fdc.disc[drive].protect) {
        result |= 0x08;
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
  if (!FdcNoDisc(bbc)) {
    spt = bbc->fdc.disc[bbc->fdc.drive].spt;
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
  if (bbc->fdc.disc[bbc->fdc.drive].protect) {
    bbc->fdc.result = FdcProtected(bbc);
    return;
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
        memcpy(bbc->fdc.disc[bbc->fdc.drive].data + off, bytes + j + 1, (size_t)size);
        wrote = true;
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
    DiscFlush(bbc, bbc->fdc.drive);
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
  if (FdcNoDisc(bbc)) {
    return 0;
  }
  spt = bbc->fdc.disc[bbc->fdc.drive].spt;
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
      out[n++] = off >= 0 ? bbc->fdc.disc[bbc->fdc.drive].data[off + k] : 0xe5;
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
      if (bbc->fdc.disc[bbc->fdc.drive].protect) {
        bbc->fdc.result = FdcProtected(bbc);
        bbc->fdc.phase = FDC_FINISH;
        bbc->fdc.delay = 200;
        return;
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

static uint8_t FdcRead(BbcMachine* bbc, uint8_t reg) {
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
    bbc->serial_ula = value;
    SerialUpdate(bbc);
    return;
  }
  if (page < 0x20) {
    // &FE18-&FE1F is the Econet ADLC. Nothing is fitted.
    return;
  }
  if (page < 0x30) {
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
    return;
  }
  if (page < 0xe0) {
    if ((page & 3) == 0) {
      AdcWrite(bbc, value);
    }
    return;
  }
  // &FEE0-&FEFF is the Tube. An empty socket does not echo the probe.
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
    return 0xfe;
  }
  if (page < 0x30) {
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
    return 0xfe;
  }
  if (page < 0xe0) {
    return AdcRead(bbc, page & 3);
  }
  return 0xfe;
}

void BbcMachineWrite(BbcMachine* bbc, uint16_t addr, uint8_t value) {
  // ACCCON bit IFJ sends &FC00-&FDFF to the cartridge port. Nothing is fitted.
  if (bbc->master && (bbc->acccon & 0x20) != 0 && addr >= 0xfc00 && addr <= 0xfdff) {
    return;
  }
  if (addr >= 0xfc00 && addr <= 0xfcff) {
    // &FCFF pages JIM. The rest of FRED is an empty 1 MHz bus: a write does
    // not stick, so a probe cannot mistake it for a Winchester board.
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
  bool graphic = bbc->tt_graphics && !control;
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
  if (control || hidden || glyph < 0x20) {
    for (pix = 0; pix < ppc; pix++) {
      PutBeam(bbc, x + pix, y, kRgb[bbc->tt_bg & 7]);
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
          PutBeam(bbc, x0 + i, y, kRgb[bbc->tt_bg & 7]);
        }
        for (i = 1; i < w - 1; i++) {
          PutBeam(bbc, x0 + i, y, colour);
        }
      } else {
        for (i = 0; i < w; i++) {
          PutBeam(bbc, x0 + i, y, colour);
        }
      }
    }
  } else {
    int rows_used = bbc->tt_double ? 4 : 8;
    int src = bbc->tt_double && bbc->tt_bottom ? 4 : 0;
    int fy = (ra * rows_used) / span;
    uint8_t bits;
    int fx;
    if (fy >= rows_used) {
      fy = rows_used - 1;
    }
    bits = kBbcFont8x8[glyph][src + fy];
    for (pix = 0; pix < ppc; pix++) {
      PutBeam(bbc, x + pix, y, kRgb[bbc->tt_bg & 7]);
    }
    for (fx = 0; fx < 8; fx++) {
      int x0;
      int x1;
      int xx;
      if ((bits & (1 << fx)) == 0) {
        continue;
      }
      x0 = x + fx * ppc / 8;
      x1 = x + (fx + 1) * ppc / 8;
      for (xx = x0; xx < x1; xx++) {
        PutBeam(bbc, xx, y, kRgb[bbc->tt_fg & 7]);
      }
    }
  }
  if (byte < 0x20) {
    TeletextApply(bbc, byte);
  } else if (bbc->tt_graphics) {
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
    bbc->beam_y++;
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
      }
    } else if (teletext) {
      uint8_t byte = VideoByte(bbc, ma, 0) & 0x7f;
      DrawBeamTeletext(bbc, bbc->disp_x, bbc->beam_y, byte, bbc->scanline);
      MaybeInvertCursor(bbc, bbc->disp_x, bbc->beam_y, width, ma, bbc->scanline);
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

void BbcMachineAdvance(BbcMachine* bbc, int cpu_cycles) {
  int via_ticks;
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
  return ViaIrq(&bbc->sys) || ViaIrq(&bbc->user) || AcaiIrq(bbc);
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

static bool ReadFile(const char* path, uint8_t** out, size_t* length) {
  char expanded[PATH_MAX];
  FILE* fp;
  // The shell does not expand ~ after the comma in -rom 15,~/file.
  if (path != NULL && path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
    const char* home = getenv("HOME");
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
  if (!ReadFile(path, &buf, &length)) {
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
  *tracks = unit > 0 ? (int)(length / (size_t)unit) : 0;
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
  for (i = 0; i < 16; i++) {
    if (!bbc->sideways_loaded[i] || bbc->sideways[i] == NULL) {
      continue;
    }
    if (RomHas(bbc->sideways[i], "ADFS") || RomHas(bbc->sideways[i], "1770")) {
      bbc->fdc.kind = BBC_FDC_1770;
      return;
    }
  }
  for (i = 0; i < 2; i++) {
    const char* path = bbc->fdc.disc[i].path;
    if (path != NULL &&
        (ExtIs(path, ".adf") || ExtIs(path, ".adm") || ExtIs(path, ".adl"))) {
      bbc->fdc.kind = BBC_FDC_1770;
      return;
    }
  }
}

static void DiscRelease(BbcMachine* bbc, int drive) {
  free(bbc->fdc.disc[drive].data);
  free(bbc->fdc.disc[drive].path);
  memset(&bbc->fdc.disc[drive], 0, sizeof(bbc->fdc.disc[drive]));
}

bool BbcMachineLoadDisc(BbcMachine* bbc, int drive, const char* path) {
  char expanded[PATH_MAX];
  uint8_t* buf = NULL;
  size_t length = 0;
  int spt = 10;
  int sides = 1;
  int tracks = 0;
  const char* stored;
  if (bbc == NULL || path == NULL || drive < 0 || drive > 1) {
    return false;
  }
  stored = path;
  if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
    const char* home = getenv("HOME");
    int written;
    if (home != NULL && home[0] != '\0') {
      written = snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
      if (written > 0 && (size_t)written < sizeof(expanded)) {
        stored = expanded;
      }
    }
  }
  if (!ReadFile(stored, &buf, &length)) {
    fprintf(stderr, "Unable to read disc image '%s'\n", path);
    return false;
  }
  DiscShape(stored, length, &spt, &sides, &tracks);
  DiscRelease(bbc, drive);
  bbc->fdc.disc[drive].data = buf;
  bbc->fdc.disc[drive].length = length;
  bbc->fdc.disc[drive].spt = spt;
  bbc->fdc.disc[drive].sides = sides;
  bbc->fdc.disc[drive].tracks = tracks;
  bbc->fdc.disc[drive].protect = access(stored, W_OK) != 0;
  bbc->fdc.disc[drive].path = strdup(stored);
  FdcDetect(bbc);
  fprintf(stderr, "Disc %d: %s (%d track%s, %d side%s, %s)\n", drive, stored, tracks,
          tracks == 1 ? "" : "s", sides, sides == 1 ? "" : "s", spt >= 16 ? "ADFS" : "DFS");
  return true;
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
  return bbc != NULL && bbc->fdc.nmi_edge;
}

void BbcMachineClearNmi(BbcMachine* bbc) {
  if (bbc != NULL) {
    bbc->fdc.nmi_edge = false;
  }
}

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
  // The new image is already in the buffer. Dropping the mapped slot skips
  // the write-back that would replace it with the old window.
  if (bbc->ram != NULL && bbc->mapped_slot == slot) {
    bbc->mapped_slot = -1;
  }
  MapRomsel(bbc);
  FdcDetect(bbc);
  return true;
}

bool BbcMachineLoadSideways(BbcMachine* bbc, int slot, const char* path) {
  uint8_t* buf = NULL;
  size_t length = 0;
  bool ok;
  if (!ReadFile(path, &buf, &length)) {
    fprintf(stderr, "Unable to read sideways ROM '%s'\n", path);
    return false;
  }
  ok = BbcMachineLoadSidewaysBytes(bbc, slot, buf, length);
  free(buf);
  return ok;
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
// are sideways sockets. Other .rom names are rejected.
static int ClassifyRomName(const char* name, int* slot) {
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
    return 1;
  }
  if (strncasecmp(stem, "os-", 3) == 0 && stem[3] != '\0') {
    *slot = -1;
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
  if (stem[i] == '\0' || (stem[i] == '-' && stem[i + 1] != '\0')) {
    *slot = value;
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

static char* RomDirectoryBeside(const char* binary, const char* directory) {
  char folder[PATH_MAX];
  char candidate[PATH_MAX];
  const char* slash;
  size_t folder_len;
  if (binary == NULL || binary[0] == '\0' || directory == NULL) {
    return NULL;
  }
  slash = strrchr(binary, '/');
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
  slash = strrchr(folder, '/');
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

int BbcMachineListRomDirectory(const char* dir, BbcRomFile* files, int capacity) {
  DIR* handle;
  struct dirent* entry;
  int count = 0;
  bool saw_os = false;
  bool saw_slot[16];
  int i;
  if (dir == NULL || files == NULL || capacity < 1) {
    return -1;
  }
  for (i = 0; i < 16; i++) {
    saw_slot[i] = false;
  }
  handle = opendir(dir);
  if (handle == NULL) {
    fprintf(stderr, "Unable to open ROM directory '%s'\n", dir);
    return -1;
  }
  while ((entry = readdir(handle)) != NULL) {
    int slot = 0;
    int kind;
    char* path;
    struct stat st;
    kind = ClassifyRomName(entry->d_name, &slot);
    if (kind == 0) {
      continue;
    }
    path = JoinPath(dir, entry->d_name);
    if (path == NULL) {
      closedir(handle);
      BbcRomFileFree(files, count);
      return -1;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
      free(path);
      if (kind < 0) {
        fprintf(stderr, "ROM file '%s' is not os[-name].rom or <socket>[-name].rom\n",
                entry->d_name);
        closedir(handle);
        BbcRomFileFree(files, count);
        return -1;
      }
      continue;
    }
    if (kind < 0) {
      fprintf(stderr, "ROM file '%s' is not os[-name].rom or <socket>[-name].rom\n",
              entry->d_name);
      free(path);
      closedir(handle);
      BbcRomFileFree(files, count);
      return -1;
    }
    if (slot < 0) {
      if (saw_os) {
        fprintf(stderr, "ROM directory '%s' has more than one OS image\n", dir);
        free(path);
        closedir(handle);
        BbcRomFileFree(files, count);
        return -1;
      }
      saw_os = true;
    } else if (saw_slot[slot]) {
      fprintf(stderr, "ROM directory '%s' has two images for socket %d\n", dir, slot);
      free(path);
      closedir(handle);
      BbcRomFileFree(files, count);
      return -1;
    } else {
      saw_slot[slot] = true;
    }
    if (count >= capacity) {
      fprintf(stderr, "ROM directory '%s' has too many images\n", dir);
      free(path);
      closedir(handle);
      BbcRomFileFree(files, count);
      return -1;
    }
    files[count].slot = slot;
    files[count].path = path;
    count++;
  }
  closedir(handle);
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
  return bbc;
}

void BbcMachineDestroy(BbcMachine* bbc) {
  int i;
  if (bbc == NULL) {
    return;
  }
  for (i = 0; i < 16; i++) {
    free(bbc->sideways[i]);
  }
  for (i = 0; i < 2; i++) {
    free(bbc->fdc.disc[i].data);
    free(bbc->fdc.disc[i].path);
  }
  if (bbc->audio_ready) {
    pthread_mutex_destroy(&bbc->audio_mu);
  }
  free(bbc->tape);
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

bool BbcMachineLoadTape(BbcMachine* bbc, const char* path) {
  uint8_t* buf = NULL;
  size_t length = 0;
  size_t i;
  if (bbc == NULL || path == NULL || !ReadFile(path, &buf, &length)) {
    return false;
  }
  free(bbc->tape);
  bbc->tape = NULL;
  bbc->tape_len = 0;
  bbc->tape_pos = 0;
  if (length >= 12 && memcmp(buf, "UEF File!", 9) == 0) {
    size_t cursor = 12;
    while (cursor + 6 <= length) {
      uint16_t id = (uint16_t)(buf[cursor] | (buf[cursor + 1] << 8));
      uint32_t chunk = (uint32_t)buf[cursor + 2] | ((uint32_t)buf[cursor + 3] << 8) |
                       ((uint32_t)buf[cursor + 4] << 16) | ((uint32_t)buf[cursor + 5] << 24);
      cursor += 6;
      if (cursor + chunk > length) {
        break;
      }
      if ((id == 0x0100 || id == 0x0110) && chunk >= 2) {
        int cycles = buf[cursor] | (buf[cursor + 1] << 8);
        int count = cycles / 20;
        int n;
        if (count < 1) {
          count = 1;
        }
        for (n = 0; n < count; n++) {
          TapeAppend(bbc, 0xaa, 1);
        }
      } else if (id == 0x0104 || id == 0x0102) {
        uint32_t n;
        for (n = 0; n < chunk; n++) {
          TapeAppend(bbc, buf[cursor + n], 0);
        }
      }
      cursor += chunk;
    }
  } else {
    for (i = 0; i < 32; i++) {
      TapeAppend(bbc, 0xaa, 1);
    }
    for (i = 0; i < length; i++) {
      TapeAppend(bbc, buf[i], 0);
    }
  }
  free(buf);
  bbc->tape_pos = 0;
  return true;
}

void BbcMachineSetKey(BbcMachine* bbc, int column, int row, bool down) {
  if (bbc == NULL || column < 0 || column > 15 || row < 0 || row > 7) {
    return;
  }
  bbc->key_down[column][row] = down ? 1 : 0;
  UpdateKeyboard(bbc);
}

int BbcMachineReadAudio(BbcMachine* bbc, int16_t* dst, int max_samples) {
  int count = 0;
  if (bbc == NULL || dst == NULL || max_samples <= 0 || !bbc->audio_ready) {
    return 0;
  }
  pthread_mutex_lock(&bbc->audio_mu);
  while (count < max_samples && bbc->audio_r != bbc->audio_w) {
    dst[count++] = bbc->audio_ring[bbc->audio_r];
    bbc->audio_r = (bbc->audio_r + 1) % BBC_AUDIO_RING;
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
