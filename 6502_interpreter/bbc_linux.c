//
//  BBC Micro emulator window for Linux. Xlib is used because a desktop
//  Ubuntu or Debian install already has libX11. Sound uses ALSA when the
//  development files were present at build time.
//

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "bbc_session.h"
#include "bbc_hardware.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#ifdef BBC_HAVE_ALSA
#include <alsa/asoundlib.h>
#include <pthread.h>
#endif

static const int kFrameCycles = 40000;

typedef struct {
  bool used;
  unsigned code;
  int column;
  int row;
  int shift_force;
  int frames;
  bool released;
} HeldKey;

static bool g_host_shift = false;
static bool g_shift_l = false;
static bool g_shift_r = false;
static bool g_ctrl_l = false;
static bool g_ctrl_r = false;
static HeldKey g_held[16];
// Typing presses stay down for two frames, then come up. The MOS toggles
// Caps Lock on the scan after it first sees the key, so the click has to
// outlast that scan. Holding the key for the whole time the light is on
// toggles only on the way on.
static int g_caps_pulse = 0;

static Display* g_dpy = NULL;
static Window g_win = 0;
static GC g_gc = 0;
static XImage* g_image = NULL;
static int g_win_w = 640;
static int g_win_h = 512;
static int g_view_x = 0;
static int g_view_y = 0;
static int g_view_w = 640;
static int g_view_h = 512;
static int g_sized_w = 0;
static int g_sized_h = 0;
static uint8_t g_prev_fb[BBC_FB_WIDTH * BBC_FB_HEIGHT * 3];
static int g_prev_src_w = 0;
static int g_prev_src_h = 0;
static int g_prev_win_w = 0;
static int g_prev_win_h = 0;
static bool g_force_full_blit = true;
static Atom g_wm_delete = 0;
static bool g_running = true;
static int g_fire0 = 0;
static int g_fire1 = 0;
static int g_ptr_x = 0;
static int g_ptr_y = 0;
static bool g_ptr_valid = false;
static int g_rshift = 0;
static int g_gshift = 0;
static int g_bshift = 0;
static int g_rbits = 8;
static int g_gbits = 8;
static int g_bbits = 8;

#ifdef BBC_HAVE_ALSA
static volatile int g_audio_run = 0;
static pthread_t g_audio_thread;
static snd_pcm_t* g_pcm = NULL;
#endif

static uint64_t MonoNs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void SyncShift(void) {
  int forced = -1;
  int i;
  if (!g_ready || g_cpu.bbc == NULL) {
    return;
  }
  for (i = 0; i < 16; i++) {
    if (g_held[i].used && g_held[i].shift_force >= 0) {
      forced = g_held[i].shift_force;
    }
  }
  BbcMachineSetKey(g_cpu.bbc, 0, 0, forced >= 0 ? forced != 0 : g_host_shift);
}

static void ReleaseHostKeyNow(unsigned code) {
  int i;
  for (i = 0; i < 16; i++) {
    if (!g_held[i].used || g_held[i].code != code) {
      continue;
    }
    BbcMachineSetKey(g_cpu.bbc, g_held[i].column, g_held[i].row, false);
    g_held[i].used = false;
    g_held[i].frames = 0;
    g_held[i].released = false;
  }
  SyncShift();
}

static void ReleaseHostKey(unsigned code) {
  int i;
  for (i = 0; i < 16; i++) {
    if (!g_held[i].used || g_held[i].code != code) {
      continue;
    }
    // Keep a tap down for two 50 Hz frames so the MOS 100 Hz keyboard
    // scanner sees it across its debounce pass even when a slow blit made
    // KeyPress and KeyRelease arrive in the same X11 batch.
    if (g_held[i].frames > 0) {
      g_held[i].released = true;
      if (g_held[i].shift_force < 0) {
        g_held[i].shift_force = g_host_shift ? 1 : 0;
      }
      continue;
    }
    BbcMachineSetKey(g_cpu.bbc, g_held[i].column, g_held[i].row, false);
    g_held[i].used = false;
  }
  SyncShift();
}

static bool KeyHoldPending(void) {
  int i;
  for (i = 0; i < 16; i++) {
    if (g_held[i].used && g_held[i].frames > 0) {
      return true;
    }
  }
  return false;
}

static void ReleaseAllKeys(void) {
  int i;
  for (i = 0; i < 16; i++) {
    if (!g_held[i].used) {
      continue;
    }
    BbcMachineSetKey(g_cpu.bbc, g_held[i].column, g_held[i].row, false);
    g_held[i].used = false;
    g_held[i].frames = 0;
    g_held[i].released = false;
  }
  g_host_shift = false;
  g_shift_l = false;
  g_shift_r = false;
  g_ctrl_l = false;
  g_ctrl_r = false;
  g_fire0 = 0;
  g_fire1 = 0;
  g_caps_pulse = 0;
  if (g_ready && g_cpu.bbc != NULL) {
    BbcMachineSetKey(g_cpu.bbc, 0, 0, false);
    BbcMachineSetKey(g_cpu.bbc, 1, 0, false);
    BbcMachineSetKey(g_cpu.bbc, 0, 4, false);
    BbcMachineSetFire(g_cpu.bbc, 0, false);
    BbcMachineSetFire(g_cpu.bbc, 1, false);
  }
}

static bool GlyphToBbc(unsigned ch, int* column, int* row, bool* need_shift) {
  static const int kDigit[10][2] = {
      {7, 2}, {0, 3}, {1, 3}, {1, 1}, {2, 1}, {3, 1}, {4, 3}, {4, 2}, {5, 1}, {6, 2},
  };
  struct {
    unsigned ch;
    int column;
    int row;
    bool shift;
  } keys[] = {
      {'!', 0, 3, true},  {'"', 1, 3, true},  {'#', 1, 1, true},  {'$', 2, 1, true},
      {'%', 3, 1, true},  {'&', 4, 3, true},  {'\'', 4, 2, true}, {'(', 5, 1, true},
      {')', 6, 2, true},  {'=', 7, 1, true},  {'~', 8, 1, true},  {'|', 8, 7, true},
      {'{', 8, 3, true},  {'+', 7, 5, true},  {'*', 8, 4, true},  {'}', 8, 5, true},
      {'<', 6, 6, true},  {'>', 7, 6, true},  {'?', 8, 6, true},  {0x00A3, 8, 2, true},
      {'`', 8, 2, true},  {'-', 7, 1, false}, {'^', 8, 1, false}, {'\\', 8, 7, false},
      {'@', 7, 4, false}, {'[', 8, 3, false}, {'_', 8, 2, false}, {';', 7, 5, false},
      {':', 8, 4, false}, {']', 8, 5, false}, {',', 6, 6, false}, {'.', 7, 6, false},
      {'/', 8, 6, false}, {' ', 2, 6, false},
  };
  size_t i;
  if (ch >= '0' && ch <= '9') {
    *column = kDigit[ch - '0'][0];
    *row = kDigit[ch - '0'][1];
    *need_shift = false;
    return true;
  }
  // Modes 0-6 draw the typed code. Shift-3 is &23 (#), column 8 row 2 is
  // &5F (_), and its shift is &60 (the pound sign). UK MODE 7 draws those
  // as the pound sign, # and a dash.
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (keys[i].ch == ch) {
      *column = keys[i].column;
      *row = keys[i].row;
      *need_shift = keys[i].shift;
      return true;
    }
  }
  return false;
}

static bool LetterToBbc(unsigned ch, int* column, int* row) {
  static const int cells[26][2] = {
      {1, 4}, {4, 6}, {2, 5}, {2, 3}, {2, 2}, {3, 4}, {3, 5}, {4, 5}, {5, 2}, {5, 4},
      {6, 4}, {6, 5}, {5, 6}, {5, 5}, {6, 3}, {7, 3}, {0, 1}, {3, 3}, {1, 5}, {3, 2},
      {5, 3}, {3, 6}, {1, 2}, {2, 4}, {4, 4}, {1, 6},
  };
  if (ch >= 'A' && ch <= 'Z') {
    ch = ch - 'A' + 'a';
  }
  if (ch < 'a' || ch > 'z') {
    return false;
  }
  *column = cells[ch - 'a'][0];
  *row = cells[ch - 'a'][1];
  return true;
}

static bool FunctionToBbc(KeySym sym, int* column, int* row) {
  static const int cells[10][2] = {
      {1, 7}, {2, 7}, {3, 7}, {4, 1}, {4, 7}, {5, 7}, {6, 1}, {6, 7}, {7, 7}, {0, 2},
  };
  int index;
  if (sym < XK_F1 || sym > XK_F10) {
    return false;
  }
  index = (int)(sym - XK_F1);
  *column = cells[index][0];
  *row = cells[index][1];
  return true;
}

static bool KeypadToBbc(KeySym sym, int* column, int* row) {
  struct {
    KeySym sym;
    int column;
    int row;
  } keys[] = {
      {XK_KP_0, 10, 6},        {XK_KP_1, 11, 6},       {XK_KP_2, 12, 7},
      {XK_KP_3, 12, 6},        {XK_KP_4, 10, 7},       {XK_KP_5, 11, 7},
      {XK_KP_6, 10, 1},        {XK_KP_7, 11, 1},       {XK_KP_8, 10, 2},
      {XK_KP_9, 11, 2},        {XK_KP_Add, 10, 3},     {XK_KP_Subtract, 11, 3},
      {XK_KP_Enter, 12, 3},          {XK_KP_Divide, 10, 4},  {XK_KP_Decimal, 12, 4},
      {XK_KP_Multiply, 11, 5}, {XK_KP_Equal, 10, 5},
  };
  size_t i;
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (keys[i].sym == sym) {
      *column = keys[i].column;
      *row = keys[i].row;
      return true;
    }
  }
  return false;
}

static bool SpecialToBbc(KeySym sym, int* column, int* row) {
  if (sym == XK_Escape) {
    *column = 0;
    *row = 7;
    return true;
  }
  if (sym == XK_Tab) {
    *column = 0;
    *row = 6;
    return true;
  }
  if (sym == XK_Return || sym == XK_KP_Enter) {
    *column = 9;
    *row = 4;
    return true;
  }
  if (sym == XK_BackSpace || sym == XK_Delete) {
    *column = 9;
    *row = 5;
    return true;
  }
  if (sym == XK_End) {
    *column = 9;
    *row = 6;
    return true;
  }
  if (sym == XK_Left) {
    *column = 9;
    *row = 1;
    return true;
  }
  if (sym == XK_Right) {
    *column = 9;
    *row = 7;
    return true;
  }
  if (sym == XK_Down) {
    *column = 9;
    *row = 2;
    return true;
  }
  if (sym == XK_Up) {
    *column = 9;
    *row = 3;
    return true;
  }
  return false;
}

static bool ModifierSym(KeySym sym) {
  return sym == XK_Shift_L || sym == XK_Shift_R || sym == XK_Control_L || sym == XK_Control_R ||
         sym == XK_Caps_Lock || sym == XK_Alt_L || sym == XK_Alt_R || sym == XK_Super_L ||
         sym == XK_Super_R || sym == XK_Meta_L || sym == XK_Meta_R || sym == XK_ISO_Level3_Shift;
}

static void PressHostKey(XKeyEvent* event) {
  KeySym sym;
  int column = 0;
  int row = 0;
  int shift_force = -1;
  bool mapped = false;
  bool control = (event->state & ControlMask) != 0;
  char text[8];
  int slot;
  int n;
  sym = XLookupKeysym(event, 0);
  if (sym == XK_F11 ||
      ((event->state & (Mod1Mask | Mod4Mask)) != 0 &&
       (sym == XK_F4 || sym == XK_q || sym == XK_Q))) {
    g_running = false;
    return;
  }
  if (sym == XK_F12) {
    W65C02InterpreterResetCpu(&g_cpu);
    return;
  }
  if (ModifierSym(sym)) {
    return;
  }
  ReleaseHostKeyNow(event->keycode);
  // The per-frame poll can lag a fast Shift+key, which would put Shift on ':'.
  g_host_shift = (event->state & ShiftMask) != 0;
  if (FunctionToBbc(sym, &column, &row)) {
    mapped = true;
  }
  if (!mapped && g_cpu.bbc != NULL && BbcMachineIsMaster(g_cpu.bbc) &&
      KeypadToBbc(sym, &column, &row)) {
    mapped = true;
  }
  if (!mapped && SpecialToBbc(sym, &column, &row)) {
    mapped = true;
  }
  if (!mapped && LetterToBbc((unsigned)sym, &column, &row)) {
    mapped = true;
  }
  if (!mapped && !control) {
    bool need_shift = false;
    unsigned ch = 0;
    n = XLookupString(event, text, (int)sizeof(text) - 1, &sym, NULL);
    if (n == 1) {
      ch = (unsigned char)text[0];
    } else if (sym == XK_sterling) {
      ch = 0xA3;
    }
    if (ch >= 32 && GlyphToBbc(ch, &column, &row, &need_shift)) {
      mapped = true;
      if (ch != ' ') {
        shift_force = need_shift ? 1 : 0;
      }
    }
  }
  if (!mapped) {
    return;
  }
  for (slot = 0; slot < 16; slot++) {
    if (!g_held[slot].used) {
      break;
    }
  }
  if (slot == 16) {
    return;
  }
  g_held[slot].used = true;
  g_held[slot].code = event->keycode;
  g_held[slot].column = column;
  g_held[slot].row = row;
  g_held[slot].shift_force = shift_force;
  g_held[slot].frames = 2;
  g_held[slot].released = false;
  // Release Shift before the key is visible. A scan in between would turn
  // the underscore key into &60.
  SyncShift();
  BbcMachineSetKey(g_cpu.bbc, column, row, true);
}

static int MaskShift(unsigned long mask) {
  int shift = 0;
  while (mask != 0 && (mask & 1ul) == 0) {
    mask >>= 1;
    shift++;
  }
  return shift;
}

static int MaskBits(unsigned long mask) {
  int bits = 0;
  while (mask != 0 && (mask & 1ul) == 0) {
    mask >>= 1;
  }
  while ((mask & 1ul) != 0) {
    bits++;
    mask >>= 1;
  }
  return bits == 0 ? 8 : bits;
}

static uint32_t PackPixel(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t pr = g_rbits >= 8 ? ((uint32_t)r << (g_rbits - 8)) : ((uint32_t)r >> (8 - g_rbits));
  uint32_t pg = g_gbits >= 8 ? ((uint32_t)g << (g_gbits - 8)) : ((uint32_t)g >> (8 - g_gbits));
  uint32_t pb = g_bbits >= 8 ? ((uint32_t)b << (g_bbits - 8)) : ((uint32_t)b >> (8 - g_bbits));
  return (pr << g_rshift) | (pg << g_gshift) | (pb << g_bshift);
}

static void ScaleRow(unsigned char* row, const uint32_t* packed, int width, int win_w) {
  const union {
    uint32_t u;
    unsigned char b[4];
  } endian = {1};
  int bpp = g_image->bits_per_pixel;
  int x;
  int sx = 0;
  int acc = 0;
  if (bpp == 32 && ((g_image->byte_order == LSBFirst && endian.b[0] == 1) ||
                    (g_image->byte_order == MSBFirst && endian.b[0] == 0))) {
    uint32_t* dst = (uint32_t*)row;
    for (x = 0; x < win_w; x++) {
      dst[x] = packed[sx];
      acc += width;
      while (acc >= win_w) {
        acc -= win_w;
        sx++;
      }
      if (sx >= width) {
        sx = width - 1;
      }
    }
    return;
  }
  for (x = 0; x < win_w; x++) {
    uint32_t pixel = packed[sx];
    if (bpp == 32) {
      unsigned char* px = row + (size_t)x * 4;
      if (g_image->byte_order == MSBFirst) {
        px[0] = (unsigned char)(pixel >> 24);
        px[1] = (unsigned char)(pixel >> 16);
        px[2] = (unsigned char)(pixel >> 8);
        px[3] = (unsigned char)pixel;
      } else {
        px[0] = (unsigned char)pixel;
        px[1] = (unsigned char)(pixel >> 8);
        px[2] = (unsigned char)(pixel >> 16);
        px[3] = (unsigned char)(pixel >> 24);
      }
    } else if (bpp == 16) {
      unsigned char* px = row + (size_t)x * 2;
      if (g_image->byte_order == MSBFirst) {
        px[0] = (unsigned char)(pixel >> 8);
        px[1] = (unsigned char)pixel;
      } else {
        px[0] = (unsigned char)pixel;
        px[1] = (unsigned char)(pixel >> 8);
      }
    } else if (bpp == 24) {
      unsigned char* px = row + (size_t)x * 3;
      if (g_image->byte_order == MSBFirst) {
        px[0] = (unsigned char)(pixel >> 16);
        px[1] = (unsigned char)(pixel >> 8);
        px[2] = (unsigned char)pixel;
      } else {
        px[0] = (unsigned char)pixel;
        px[1] = (unsigned char)(pixel >> 8);
        px[2] = (unsigned char)(pixel >> 16);
      }
    }
    acc += width;
    while (acc >= win_w) {
      acc -= win_w;
      sx++;
    }
    if (sx >= width) {
      sx = width - 1;
    }
  }
}

static void DestroyImage(void) {
  if (g_image != NULL) {
    XDestroyImage(g_image);
    g_image = NULL;
  }
}

static void ComputeViewport(int win_w, int win_h, int* vx, int* vy, int* vw, int* vh) {
  if (!BbcSessionFullscreen()) {
    *vx = 0;
    *vy = 0;
    *vw = win_w < 1 ? 1 : win_w;
    *vh = win_h < 1 ? 1 : win_h;
    return;
  }
  // Fit a 4:3 CRT picture in the middle of a widescreen monitor.
  if (win_w * 3 >= win_h * 4) {
    *vh = win_h;
    *vw = (win_h * 4 + 1) / 3;
  } else {
    *vw = win_w;
    *vh = (win_w * 3 + 2) / 4;
  }
  if (*vw < 1) {
    *vw = 1;
  }
  if (*vh < 1) {
    *vh = 1;
  }
  *vx = (win_w - *vw) / 2;
  *vy = (win_h - *vh) / 2;
}

static bool CreateImage(int width, int height) {
  Visual* visual;
  int depth;
  char* data;
  int bytes;
  int vx, vy, vw, vh;
  if (width < 1 || height < 1) {
    return false;
  }
  ComputeViewport(width, height, &vx, &vy, &vw, &vh);
  visual = DefaultVisual(g_dpy, DefaultScreen(g_dpy));
  depth = DefaultDepth(g_dpy, DefaultScreen(g_dpy));
  bytes = vw * 4 * vh;
  data = calloc((size_t)bytes, 1);
  if (data == NULL) {
    return false;
  }
  DestroyImage();
  g_image = XCreateImage(g_dpy, visual, (unsigned)depth, ZPixmap, 0, data, (unsigned)vw,
                         (unsigned)vh, 32, 0);
  if (g_image == NULL) {
    free(data);
    return false;
  }
  g_win_w = width;
  g_win_h = height;
  g_view_x = vx;
  g_view_y = vy;
  g_view_w = vw;
  g_view_h = vh;
  g_force_full_blit = true;
  return true;
}

static void BlitFrame(void) {
  int width;
  int height;
  const uint8_t* src;
  bool full;
  int dirty_s0;
  int dirty_s1;
  size_t row_bytes;
  int sy;
  int y;
  int dst_y0 = -1;
  int dst_y1 = -1;
  int prev_sy = -1;
  unsigned char* prev_row = NULL;
  uint32_t packed[BBC_FB_WIDTH];
  if (g_cpu.bbc == NULL || g_image == NULL || g_view_w <= 0 || g_view_h <= 0) {
    return;
  }
  BbcMachinePresentFrame(g_cpu.bbc, g_cpu.turbo);
  width = BbcFrameWidth(g_cpu.bbc);
  height = BbcFrameHeight(g_cpu.bbc);
  if (width <= 0 || height <= 0) {
    return;
  }
  if (width > BBC_FB_WIDTH) {
    width = BBC_FB_WIDTH;
  }
  if (height > BBC_FB_HEIGHT) {
    height = BBC_FB_HEIGHT;
  }
  src = BbcFramebuffer(g_cpu.bbc);
  full = g_force_full_blit || width != g_prev_src_w || height != g_prev_src_h ||
         g_view_w != g_prev_win_w || g_view_h != g_prev_win_h;
  dirty_s0 = full ? 0 : height;
  dirty_s1 = full ? height - 1 : -1;
  row_bytes = (size_t)width * 3;
  for (sy = 0; sy < height; sy++) {
    const uint8_t* srow = src + (size_t)sy * BBC_FB_WIDTH * 3;
    uint8_t* prow = g_prev_fb + (size_t)sy * BBC_FB_WIDTH * 3;
    if (full || memcmp(srow, prow, row_bytes) != 0) {
      memcpy(prow, srow, row_bytes);
      if (sy < dirty_s0) {
        dirty_s0 = sy;
      }
      dirty_s1 = sy;
    }
  }
  g_prev_src_w = width;
  g_prev_src_h = height;
  g_prev_win_w = g_view_w;
  g_prev_win_h = g_view_h;
  g_force_full_blit = false;
  if (dirty_s0 > dirty_s1) {
    return;
  }
  for (y = 0; y < g_view_h; y++) {
    unsigned char* row;
    sy = y * height / g_view_h;
    if (sy >= height) {
      sy = height - 1;
    }
    if (sy < dirty_s0 || sy > dirty_s1) {
      continue;
    }
    if (dst_y0 < 0) {
      dst_y0 = y;
    }
    dst_y1 = y;
    row = (unsigned char*)g_image->data + (size_t)y * (size_t)g_image->bytes_per_line;
    if (sy == prev_sy && prev_row != NULL) {
      memcpy(row, prev_row, (size_t)g_image->bytes_per_line);
      continue;
    }
    {
      const uint8_t* srow = src + (size_t)sy * BBC_FB_WIDTH * 3;
      int sx;
      for (sx = 0; sx < width; sx++) {
        const uint8_t* p = srow + (size_t)sx * 3;
        packed[sx] = PackPixel(p[0], p[1], p[2]);
      }
    }
    ScaleRow(row, packed, width, g_view_w);
    prev_sy = sy;
    prev_row = row;
  }
  if (dst_y0 >= 0 && dst_y1 >= dst_y0) {
    XPutImage(g_dpy, g_win, g_gc, g_image, 0, dst_y0, g_view_x, g_view_y + dst_y0,
              (unsigned)g_view_w, (unsigned)(dst_y1 - dst_y0 + 1));
    XSync(g_dpy, False);
  }
}

static void UpdateTitle(void) {
  static int shown = -1;
  int caps;
  int shift;
  int motor;
  int cmos;
  int master;
  int stamp;
  char title[128];
  if (g_cpu.bbc == NULL) {
    return;
  }
  caps = BbcMachineCapsLed(g_cpu.bbc) ? 1 : 0;
  shift = BbcMachineShiftLed(g_cpu.bbc) ? 1 : 0;
  motor = BbcMachineMotorOn(g_cpu.bbc) ? 1 : 0;
  cmos = g_cpu.bbc_65c02 ? 1 : 0;
  master = BbcMachineIsMaster(g_cpu.bbc) ? 1 : 0;
  stamp = caps | (shift << 1) | (motor << 2) | (cmos << 3) | (master << 4);
  if (stamp == shown) {
    return;
  }
  snprintf(title, sizeof(title), "%s%s%s%s%s", master ? "BBC Master" : "BBC Micro",
           caps ? "  CAPS" : "", shift ? "  SHIFT" : "", motor ? "  MOTOR" : "",
           master ? "  65SC12" : (cmos ? "  65C02" : ""));
  XStoreName(g_dpy, g_win, title);
  shown = stamp;
}

// -scale multiplies the picture. 1 leaves each displayed pixel one window
// pixel across, after the bitmap modes have been stretched to look square.
static int ScaledLength(int size) {
  double px;
  int n;
  if (size < 1) {
    return 1;
  }
  px = (double)size * BbcSessionScreenScale();
  n = (int)(px + 0.5);
  if (n < 1) {
    n = 1;
  }
  return n;
}

static void FitWindow(int width, int height) {
  int want_h;
  int win_w;
  int win_h;
  int min_w;
  int min_h;
  XSizeHints hints;
  if (BbcSessionFullscreen() || width <= 0 || height <= 0) {
    return;
  }
  if (width == g_sized_w && abs(height - g_sized_h) <= 8) {
    return;
  }
  g_sized_w = width;
  g_sized_h = height;
  // Bitmap pixels are twice as tall as they are wide. MODE 7 already
  // stores both rounding halves, so a tall frame is left square.
  want_h = height * (height > 400 ? 1 : 2);
  if (want_h < 1) {
    want_h = 1;
  }
  win_w = ScaledLength(width);
  win_h = ScaledLength(want_h);
  // 400x320 is the usual smallest size. A scale that asks for less than
  // that follows the requested picture, as on the Mac window.
  min_w = 400;
  min_h = 320;
  if (win_w < min_w || win_h < min_h) {
    min_w = win_w;
    min_h = win_h;
  }
  memset(&hints, 0, sizeof(hints));
  hints.flags = PMinSize | PAspect;
  hints.min_width = min_w;
  hints.min_height = min_h;
  hints.min_aspect.x = width;
  hints.min_aspect.y = want_h;
  hints.max_aspect = hints.min_aspect;
  XSetWMNormalHints(g_dpy, g_win, &hints);
  if (win_w != g_win_w || win_h != g_win_h) {
    XResizeWindow(g_dpy, g_win, (unsigned)win_w, (unsigned)win_h);
    CreateImage(win_w, win_h);
  }
}

static void QueryPointerOnce(void) {
  Window root;
  Window child;
  int rx;
  int ry;
  int x;
  int y;
  unsigned mask;
  if (g_dpy == NULL || g_win == 0) {
    return;
  }
  if (!XQueryPointer(g_dpy, g_win, &root, &child, &rx, &ry, &x, &y, &mask)) {
    return;
  }
  g_ptr_x = x;
  g_ptr_y = y;
  g_ptr_valid = x >= 0 && y >= 0 && x < g_win_w && y < g_win_h;
  if (!KeyHoldPending()) {
    g_host_shift = (mask & ShiftMask) != 0;
    SyncShift();
  }
  if (g_cpu.bbc != NULL) {
    BbcMachineSetKey(g_cpu.bbc, 1, 0, (mask & ControlMask) != 0);
  }
}

static void UpdatePointer(void) {
  int vx = g_ptr_x - g_view_x;
  int vy = g_ptr_y - g_view_y;
  if (g_cpu.bbc == NULL || BbcMachineJoystickRemoteActive(g_cpu.bbc)) {
    return;
  }
  if (g_ptr_valid && vx >= 0 && vy >= 0 && vx < g_view_w && vy < g_view_h && g_view_w > 1 &&
      g_view_h > 1) {
    BbcMachineSetAnalogue(g_cpu.bbc, 0, vx * 65535 / (g_view_w - 1));
    BbcMachineSetAnalogue(g_cpu.bbc, 1, vy * 65535 / (g_view_h - 1));
  } else {
    BbcMachineSetAnalogue(g_cpu.bbc, 0, 0x8000);
    BbcMachineSetAnalogue(g_cpu.bbc, 1, 0x8000);
  }
  BbcMachineSetFire(g_cpu.bbc, 0, g_fire0 != 0);
  BbcMachineSetFire(g_cpu.bbc, 1, g_fire1 != 0);
}

static void Tick(bool draw) {
  int cycles = 0;
  int budget = kFrameCycles;
  int limit;
  int i;
  bool released_any = false;
  uint64_t started;
  if (!g_ready || !g_cpu.running) {
    g_running = false;
    return;
  }
  UpdatePointer();
  if (g_cpu.clock_mhz > 0) {
    budget = kFrameCycles * g_cpu.clock_mhz / 2;
    if (budget < 1) {
      budget = 1;
    }
  }
  limit = g_cpu.turbo ? 2000000 : budget;
  started = MonoNs();
  while (cycles < limit && g_cpu.running) {
    int step = W65C02InterpreterStep(&g_cpu);
    if (step <= 0) {
      break;
    }
    cycles += step;
    if (!g_cpu.turbo && cycles >= budget) {
      break;
    }
    if (g_cpu.turbo && cycles > kFrameCycles && MonoNs() - started > 8000000ull) {
      break;
    }
  }
  if (g_cpu.bbc != NULL) {
    for (i = 0; i < 16; i++) {
      if (!g_held[i].used || g_held[i].frames <= 0) {
        continue;
      }
      g_held[i].frames--;
      if (g_held[i].frames == 0 && g_held[i].released) {
        BbcMachineSetKey(g_cpu.bbc, g_held[i].column, g_held[i].row, false);
        g_held[i].used = false;
        released_any = true;
      }
    }
    if (released_any) {
      SyncShift();
    }
  }
  if (g_caps_pulse > 0 && g_cpu.bbc != NULL && !BbcSessionGameCaps()) {
    g_caps_pulse--;
    if (g_caps_pulse == 0) {
      BbcMachineSetKey(g_cpu.bbc, 0, 4, false);
    }
  }
  if (g_cpu.bbc == NULL) {
    return;
  }
  UpdateTitle();
  if (!draw) {
    return;
  }
  if (BbcFrameWidth(g_cpu.bbc) > 0 && BbcFrameHeight(g_cpu.bbc) > 0) {
    FitWindow(BbcFrameWidth(g_cpu.bbc), BbcFrameHeight(g_cpu.bbc));
  }
  BlitFrame();
}

static void OnKey(XKeyEvent* event, bool down) {
  KeySym sym = XLookupKeysym(event, 0);
  bool caps = sym == XK_Caps_Lock;
  if (sym == XK_Shift_L || sym == XK_Shift_R) {
    if (sym == XK_Shift_L) {
      g_shift_l = down;
    } else {
      g_shift_r = down;
    }
    g_host_shift = g_shift_l || g_shift_r;
    SyncShift();
    return;
  }
  if (sym == XK_Control_L || sym == XK_Control_R) {
    if (sym == XK_Control_L) {
      g_ctrl_l = down;
    } else {
      g_ctrl_r = down;
    }
    if (g_cpu.bbc != NULL) {
      BbcMachineSetKey(g_cpu.bbc, 1, 0, g_ctrl_l || g_ctrl_r);
    }
    return;
  }
  if (!down) {
    XEvent next;
    if (XEventsQueued(g_dpy, QueuedAfterReading) > 0) {
      XPeekEvent(g_dpy, &next);
      if (next.type == KeyPress && next.xkey.keycode == event->keycode &&
          next.xkey.time == event->time) {
        XNextEvent(g_dpy, &next);
        return;
      }
    }
    if (caps) {
      if (BbcSessionGameCaps() && g_cpu.bbc != NULL) {
        BbcMachineSetKey(g_cpu.bbc, 0, 4, false);
      }
      return;
    }
    ReleaseHostKey(event->keycode);
    return;
  }
  if (caps) {
    if (g_cpu.bbc == NULL) {
      return;
    }
    BbcMachineSetKey(g_cpu.bbc, 0, 4, true);
    if (!BbcSessionGameCaps()) {
      g_caps_pulse = 2;
    }
    return;
  }
  {
    int i;
    for (i = 0; i < 16; i++) {
      if (g_held[i].used && !g_held[i].released && g_held[i].code == event->keycode) {
        return;
      }
    }
  }
  PressHostKey(event);
}

static void HandleEvent(XEvent* event) {
  if (event->type == ClientMessage && (Atom)event->xclient.data.l[0] == g_wm_delete) {
    g_running = false;
    return;
  }
  if (event->type == ConfigureNotify) {
    int w = event->xconfigure.width;
    int h = event->xconfigure.height;
    if (w != g_win_w || h != g_win_h) {
      CreateImage(w, h);
    }
    return;
  }
  if (event->type == Expose && event->xexpose.count == 0 && g_image != NULL) {
    if (g_force_full_blit) {
      BlitFrame();
    } else {
      XPutImage(g_dpy, g_win, g_gc, g_image, 0, 0, g_view_x, g_view_y, (unsigned)g_view_w,
                (unsigned)g_view_h);
    }
    return;
  }
  if (event->type == FocusIn) {
    QueryPointerOnce();
    return;
  }
  if (event->type == FocusOut) {
    ReleaseAllKeys();
    return;
  }
  if (event->type == MotionNotify) {
    g_ptr_x = event->xmotion.x;
    g_ptr_y = event->xmotion.y;
    g_ptr_valid = true;
    return;
  }
  if (event->type == EnterNotify) {
    g_ptr_x = event->xcrossing.x;
    g_ptr_y = event->xcrossing.y;
    g_ptr_valid = true;
    return;
  }
  if (event->type == LeaveNotify) {
    g_ptr_valid = false;
    return;
  }
  if (event->type == KeyPress) {
    OnKey(&event->xkey, true);
    return;
  }
  if (event->type == KeyRelease) {
    OnKey(&event->xkey, false);
    return;
  }
  if (event->type == ButtonPress) {
    if (event->xbutton.button == 1) {
      g_fire0 = 1;
    } else if (event->xbutton.button == 3) {
      g_fire1 = 1;
    }
    return;
  }
  if (event->type == ButtonRelease) {
    if (event->xbutton.button == 1) {
      g_fire0 = 0;
    } else if (event->xbutton.button == 3) {
      g_fire1 = 0;
    }
  }
}

#ifdef BBC_HAVE_ALSA
static void* AudioThread(void* unused) {
  int16_t samples[1024];
  (void)unused;
  while (g_audio_run) {
    int frames = 1024;
    int got = 0;
    snd_pcm_sframes_t wrote;
    if (g_ready && g_cpu.bbc != NULL) {
      got = BbcMachineReadAudio(g_cpu.bbc, samples, frames);
    }
    if (got < frames) {
      memset(samples + got, 0, (size_t)(frames - got) * sizeof(int16_t));
    }
    wrote = snd_pcm_writei(g_pcm, samples, (snd_pcm_uframes_t)frames);
    if (wrote < 0) {
      snd_pcm_recover(g_pcm, (int)wrote, 1);
    }
  }
  return NULL;
}

static void StartAudio(void) {
  int mhz = g_cpu.clock_mhz > 0 ? g_cpu.clock_mhz : 2;
  unsigned rate = (unsigned)(48000 * mhz / 2);
  int err;
  err = snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    fprintf(stderr, "ALSA: unable to open the default sound device (%s)\n", snd_strerror(err));
    g_pcm = NULL;
    return;
  }
  err = snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, rate, 1,
                           40000);
  if (err < 0 && rate != 48000) {
    rate = 48000;
    err = snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, rate, 1,
                             40000);
  }
  if (err < 0) {
    fprintf(stderr, "ALSA: unable to set the sound format (%s)\n", snd_strerror(err));
    snd_pcm_close(g_pcm);
    g_pcm = NULL;
    return;
  }
  g_audio_run = 1;
  if (pthread_create(&g_audio_thread, NULL, AudioThread, NULL) != 0) {
    g_audio_run = 0;
    snd_pcm_close(g_pcm);
    g_pcm = NULL;
  }
}

static void StopAudio(void) {
  if (!g_audio_run) {
    return;
  }
  g_audio_run = 0;
  pthread_join(g_audio_thread, NULL);
  if (g_pcm != NULL) {
    snd_pcm_close(g_pcm);
    g_pcm = NULL;
  }
}
#else
static void StartAudio(void) {}
static void StopAudio(void) {}
#endif

static void HideCursor(void) {
  static char zero[8] = {0};
  Pixmap blank;
  XColor black;
  Cursor cursor;
  memset(&black, 0, sizeof(black));
  blank = XCreateBitmapFromData(g_dpy, g_win, zero, 8, 8);
  if (blank == None) {
    return;
  }
  cursor = XCreatePixmapCursor(g_dpy, blank, blank, &black, &black, 0, 0);
  XDefineCursor(g_dpy, g_win, cursor);
  XFreePixmap(g_dpy, blank);
}

static void EnterFullscreen(int screen, int width, int height) {
  struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
  } motif = {2, 0, 0, 0, 0};
  Atom motif_atom = XInternAtom(g_dpy, "_MOTIF_WM_HINTS", False);
  Atom wm_state = XInternAtom(g_dpy, "_NET_WM_STATE", False);
  Atom fullscreen = XInternAtom(g_dpy, "_NET_WM_STATE_FULLSCREEN", False);
  XEvent ev;
  XChangeProperty(g_dpy, g_win, motif_atom, motif_atom, 32, PropModeReplace,
                  (const unsigned char*)&motif, 5);
  XChangeProperty(g_dpy, g_win, wm_state, XA_ATOM, 32, PropModeReplace,
                  (const unsigned char*)&fullscreen, 1);
  XMapWindow(g_dpy, g_win);
  XMoveResizeWindow(g_dpy, g_win, 0, 0, (unsigned)width, (unsigned)height);
  memset(&ev, 0, sizeof(ev));
  ev.type = ClientMessage;
  ev.xclient.window = g_win;
  ev.xclient.message_type = wm_state;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = 1;
  ev.xclient.data.l[1] = (long)fullscreen;
  XSendEvent(g_dpy, RootWindow(g_dpy, screen), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  HideCursor();
}

static bool OpenWindow(void) {
  int screen;
  Visual* visual;
  XSetWindowAttributes attr;
  unsigned long mask;
  int init_w;
  int init_h;
  if (g_dpy == NULL) {
    return false;
  }
  screen = DefaultScreen(g_dpy);
  visual = DefaultVisual(g_dpy, screen);
  if (visual->class != TrueColor && visual->class != DirectColor) {
    fprintf(stderr, "bbc: the X display is not a colour screen\n");
    return false;
  }
  g_rshift = MaskShift(visual->red_mask);
  g_gshift = MaskShift(visual->green_mask);
  g_bshift = MaskShift(visual->blue_mask);
  g_rbits = MaskBits(visual->red_mask);
  g_gbits = MaskBits(visual->green_mask);
  g_bbits = MaskBits(visual->blue_mask);
  if (BbcSessionFullscreen()) {
    BbcSessionMonitorSize(&init_w, &init_h);
    if (init_w <= 0 || init_h <= 0) {
      init_w = DisplayWidth(g_dpy, screen);
      init_h = DisplayHeight(g_dpy, screen);
    }
  } else {
    init_w = ScaledLength(640);
    init_h = ScaledLength(512);
  }
  memset(&attr, 0, sizeof(attr));
  attr.background_pixel = BlackPixel(g_dpy, screen);
  attr.event_mask = KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask |
                    ButtonPressMask | ButtonReleaseMask | FocusChangeMask | PointerMotionMask |
                    EnterWindowMask | LeaveWindowMask;
  mask = CWBackPixel | CWEventMask;
  g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, screen), 0, 0, (unsigned)init_w, (unsigned)init_h,
                        0, DefaultDepth(g_dpy, screen), InputOutput, visual, mask, &attr);
  if (g_win == 0) {
    return false;
  }
  g_wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(g_dpy, g_win, &g_wm_delete, 1);
  XStoreName(g_dpy, g_win, "BBC Micro");
  g_gc = XCreateGC(g_dpy, g_win, 0, NULL);
  if (!CreateImage(init_w, init_h)) {
    return false;
  }
  if (BbcSessionFullscreen()) {
    EnterFullscreen(screen, init_w, init_h);
  } else {
    XMapWindow(g_dpy, g_win);
  }
  XFlush(g_dpy);
  QueryPointerOnce();
  return true;
}

static int HostKeyboard(Display* display) {
  XkbDescPtr desc;
  XkbStateRec state;
  char* symbols;
  const char* mark;
  int group;
  int seen;
  if (display == NULL || XkbGetState(display, XkbUseCoreKbd, &state) != Success) {
    return BBC_KEYBOARD_UK;
  }
  desc = XkbGetKeyboard(display, XkbSymbolsNameMask, XkbUseCoreKbd);
  if (desc == NULL || desc->names == NULL || desc->names->symbols == None) {
    if (desc != NULL) {
      XkbFreeKeyboard(desc, 0, True);
    }
    return BBC_KEYBOARD_UK;
  }
  symbols = XGetAtomName(display, desc->names->symbols);
  XkbFreeKeyboard(desc, 0, True);
  if (symbols == NULL) {
    return BBC_KEYBOARD_UK;
  }
  group = 0;
  seen = 0;
  mark = symbols;
  while (*mark != '\0') {
    const char* start;
    size_t n;
    while (*mark == '+' || *mark == '_') {
      mark++;
    }
    start = mark;
    while (*mark != '\0' && *mark != '+' && *mark != '_') {
      mark++;
    }
    n = (size_t)(mark - start);
    if ((n == 2 && start[0] == 'p' && start[1] == 'c') ||
        (n >= 3 && strncmp(start, "pc(", 3) == 0)) {
      continue;
    }
    if (n >= 4 && strncmp(start, "inet", 4) == 0) {
      continue;
    }
    if (n >= 5 && strncmp(start, "evdev", 5) == 0) {
      continue;
    }
    if (seen == (int)state.group) {
      group = (n >= 2 && (strncmp(start, "gb", 2) == 0 || strncmp(start, "uk", 2) == 0))
                  ? BBC_KEYBOARD_UK
                  : (n >= 2 && strncmp(start, "us", 2) == 0) ? BBC_KEYBOARD_US : BBC_KEYBOARD_UK;
      break;
    }
    seen++;
  }
  XFree(symbols);
  return group;
}

int main(int argc, char** argv) {
  int status = 1;
  int xfd;
  uint64_t next;
  if (!BbcSessionStart(argc, argv, &status)) {
    return status;
  }
  g_dpy = XOpenDisplay(NULL);
  if (g_dpy == NULL) {
    fprintf(stderr, "bbc: no X display. Start this from a desktop session.\n");
    BbcSessionFinish();
    return 1;
  }
  BbcSessionPreferKeyboard(HostKeyboard(g_dpy));
  if (!OpenWindow()) {
    fprintf(stderr, "bbc: unable to open the window\n");
    if (g_dpy != NULL) {
      XCloseDisplay(g_dpy);
    }
    BbcSessionFinish();
    return 1;
  }
  StartAudio();
  xfd = ConnectionNumber(g_dpy);
  next = MonoNs();
  {
    int skipped = 0;
    while (g_running) {
      while (XPending(g_dpy) > 0) {
        XEvent event;
        if (KeyHoldPending()) {
          XPeekEvent(g_dpy, &event);
          if (event.type == KeyPress) {
            KeySym sym = XLookupKeysym(&event.xkey, 0);
            if (!ModifierSym(sym)) {
              break;
            }
          }
        }
        XNextEvent(g_dpy, &event);
        HandleEvent(&event);
      }
      if (!g_running) {
        break;
      }
      {
        uint64_t now = MonoNs();
        if (now >= next) {
          bool behind = now >= next + 20000000ull && skipped < 3;
          Tick(!behind);
          if (behind) {
            skipped++;
          } else {
            skipped = 0;
          }
          next += 20000000ull;
          if (MonoNs() > next + 100000000ull) {
            next = MonoNs();
          }
        } else {
          struct timeval tv;
          fd_set fds;
          uint64_t wait = next - now;
          FD_ZERO(&fds);
          FD_SET(xfd, &fds);
          tv.tv_sec = (time_t)(wait / 1000000000ull);
          tv.tv_usec = (suseconds_t)((wait % 1000000000ull) / 1000ull);
          select(xfd + 1, &fds, NULL, NULL, &tv);
        }
      }
    }
  }
  StopAudio();
  DestroyImage();
  if (g_gc != 0) {
    XFreeGC(g_dpy, g_gc);
  }
  XCloseDisplay(g_dpy);
  BbcSessionFinish();
  return 0;
}
