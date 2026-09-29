//
//  BBC Micro emulator window for Linux. Xlib is used because a desktop
//  Ubuntu or Debian install already has libX11. Sound uses ALSA when the
//  development files were present at build time.
//

// The loader uses the name Region, and so does Xutil.h. Rename the X11
// typedef for this file; none of the calls below use an X region.
#define Region X11Region
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#undef Region

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
} HeldKey;

static bool g_host_shift = false;
static HeldKey g_held[16];

static Display* g_dpy = NULL;
static Window g_win = 0;
static GC g_gc = 0;
static XImage* g_image = NULL;
static int g_win_w = 640;
static int g_win_h = 512;
static int g_sized_w = 0;
static int g_sized_h = 0;
static Atom g_wm_delete = 0;
static bool g_running = true;
static int g_fire0 = 0;
static int g_fire1 = 0;
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

static void ReleaseHostKey(unsigned code) {
  int i;
  for (i = 0; i < 16; i++) {
    if (!g_held[i].used || g_held[i].code != code) {
      continue;
    }
    BbcMachineSetKey(g_cpu.bbc, g_held[i].column, g_held[i].row, false);
    g_held[i].used = false;
  }
  SyncShift();
}

static void ReleaseAllKeys(void) {
  int i;
  for (i = 0; i < 16; i++) {
    if (!g_held[i].used) {
      continue;
    }
    BbcMachineSetKey(g_cpu.bbc, g_held[i].column, g_held[i].row, false);
    g_held[i].used = false;
  }
  g_host_shift = false;
  g_fire0 = 0;
  g_fire1 = 0;
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
  if (sym == XK_F12) {
    W65C02InterpreterResetCpu(&g_cpu);
    return;
  }
  if (ModifierSym(sym)) {
    return;
  }
  ReleaseHostKey(event->keycode);
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
      if (ch != ' ' && need_shift != g_host_shift) {
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
  BbcMachineSetKey(g_cpu.bbc, column, row, true);
  SyncShift();
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

static void StorePixel(int x, int y, uint32_t pixel) {
  unsigned char* row;
  int bpp;
  if (g_image == NULL || x < 0 || y < 0 || x >= g_image->width || y >= g_image->height) {
    return;
  }
  bpp = g_image->bits_per_pixel;
  row = (unsigned char*)g_image->data + (size_t)y * (size_t)g_image->bytes_per_line;
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
}

static void DestroyImage(void) {
  if (g_image != NULL) {
    XDestroyImage(g_image);
    g_image = NULL;
  }
}

static bool CreateImage(int width, int height) {
  Visual* visual;
  int depth;
  char* data;
  int bytes;
  if (width < 1 || height < 1) {
    return false;
  }
  visual = DefaultVisual(g_dpy, DefaultScreen(g_dpy));
  depth = DefaultDepth(g_dpy, DefaultScreen(g_dpy));
  bytes = width * 4 * height;
  data = calloc((size_t)bytes, 1);
  if (data == NULL) {
    return false;
  }
  DestroyImage();
  g_image = XCreateImage(g_dpy, visual, (unsigned)depth, ZPixmap, 0, data, (unsigned)width,
                         (unsigned)height, 32, 0);
  if (g_image == NULL) {
    free(data);
    return false;
  }
  g_win_w = width;
  g_win_h = height;
  return true;
}

static void BlitFrame(void) {
  int width;
  int height;
  const uint8_t* src;
  int y;
  if (g_cpu.bbc == NULL || g_image == NULL) {
    return;
  }
  if (g_cpu.turbo || BbcMachineCompletedFrames(g_cpu.bbc) == 0) {
    BbcMachineRender(g_cpu.bbc);
  }
  width = BbcFrameWidth(g_cpu.bbc);
  height = BbcFrameHeight(g_cpu.bbc);
  if (width <= 0 || height <= 0) {
    return;
  }
  src = BbcFramebuffer(g_cpu.bbc);
  for (y = 0; y < g_win_h; y++) {
    int sy = y * height / g_win_h;
    int x;
    if (sy >= height) {
      sy = height - 1;
    }
    for (x = 0; x < g_win_w; x++) {
      int sx = x * width / g_win_w;
      const uint8_t* p;
      if (sx >= width) {
        sx = width - 1;
      }
      p = src + ((size_t)sy * BBC_FB_WIDTH + (size_t)sx) * 3;
      StorePixel(x, y, PackPixel(p[0], p[1], p[2]));
    }
  }
  XPutImage(g_dpy, g_win, g_gc, g_image, 0, 0, 0, 0, (unsigned)g_win_w, (unsigned)g_win_h);
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

static void FitWindow(int width, int height) {
  int want_h;
  XSizeHints hints;
  if (width <= 0 || height <= 0) {
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
  memset(&hints, 0, sizeof(hints));
  hints.flags = PMinSize | PAspect;
  hints.min_width = 400;
  hints.min_height = 320;
  hints.min_aspect.x = width;
  hints.min_aspect.y = want_h;
  hints.max_aspect = hints.min_aspect;
  XSetWMNormalHints(g_dpy, g_win, &hints);
  XResizeWindow(g_dpy, g_win, (unsigned)width, (unsigned)want_h);
}

static void ReadPointer(void) {
  Window root;
  Window child;
  int rx;
  int ry;
  int x;
  int y;
  unsigned mask;
  if (g_cpu.bbc == NULL) {
    return;
  }
  if (!XQueryPointer(g_dpy, g_win, &root, &child, &rx, &ry, &x, &y, &mask)) {
    return;
  }
  g_host_shift = (mask & ShiftMask) != 0;
  SyncShift();
  BbcMachineSetKey(g_cpu.bbc, 1, 0, (mask & ControlMask) != 0);
  BbcMachineSetKey(g_cpu.bbc, 0, 4, (mask & LockMask) != 0);
  if (x >= 0 && y >= 0 && x < g_win_w && y < g_win_h && g_win_w > 1 && g_win_h > 1) {
    BbcMachineSetAnalogue(g_cpu.bbc, 0, x * 65535 / (g_win_w - 1));
    BbcMachineSetAnalogue(g_cpu.bbc, 1, y * 65535 / (g_win_h - 1));
  } else {
    BbcMachineSetAnalogue(g_cpu.bbc, 0, 0x8000);
    BbcMachineSetAnalogue(g_cpu.bbc, 1, 0x8000);
  }
  BbcMachineSetFire(g_cpu.bbc, 0, g_fire0 || (mask & Button1Mask) != 0);
  BbcMachineSetFire(g_cpu.bbc, 1, g_fire1 || (mask & Button3Mask) != 0);
}

static void Tick(void) {
  int cycles = 0;
  int budget = kFrameCycles;
  int limit;
  uint64_t started;
  if (!g_ready || !g_cpu.running) {
    g_running = false;
    return;
  }
  ReadPointer();
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
  if (g_cpu.bbc == NULL) {
    return;
  }
  UpdateTitle();
  if (BbcFrameWidth(g_cpu.bbc) > 0 && BbcFrameHeight(g_cpu.bbc) > 0) {
    FitWindow(BbcFrameWidth(g_cpu.bbc), BbcFrameHeight(g_cpu.bbc));
  }
  BlitFrame();
  XFlush(g_dpy);
}

static void OnKey(XKeyEvent* event, bool down) {
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
    ReleaseHostKey(event->keycode);
    return;
  }
  {
    int i;
    for (i = 0; i < 16; i++) {
      if (g_held[i].used && g_held[i].code == event->keycode) {
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
    XPutImage(g_dpy, g_win, g_gc, g_image, 0, 0, 0, 0, (unsigned)g_win_w, (unsigned)g_win_h);
    return;
  }
  if (event->type == FocusOut) {
    ReleaseAllKeys();
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

static bool OpenWindow(void) {
  int screen;
  Visual* visual;
  XSetWindowAttributes attr;
  unsigned long mask;
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
  memset(&attr, 0, sizeof(attr));
  attr.background_pixel = BlackPixel(g_dpy, screen);
  attr.event_mask = KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask |
                    ButtonPressMask | ButtonReleaseMask | FocusChangeMask | PointerMotionMask;
  mask = CWBackPixel | CWEventMask;
  g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, screen), 0, 0, 640, 512, 0,
                        DefaultDepth(g_dpy, screen), InputOutput, visual, mask, &attr);
  if (g_win == 0) {
    return false;
  }
  g_wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(g_dpy, g_win, &g_wm_delete, 1);
  XStoreName(g_dpy, g_win, "BBC Micro");
  g_gc = XCreateGC(g_dpy, g_win, 0, NULL);
  if (!CreateImage(640, 512)) {
    return false;
  }
  XMapWindow(g_dpy, g_win);
  XFlush(g_dpy);
  return true;
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
  while (g_running) {
    while (XPending(g_dpy) > 0) {
      XEvent event;
      XNextEvent(g_dpy, &event);
      HandleEvent(&event);
    }
    if (!g_running) {
      break;
    }
    {
      uint64_t now = MonoNs();
      if (now >= next) {
        Tick();
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
  StopAudio();
  DestroyImage();
  if (g_gc != 0) {
    XFreeGC(g_dpy, g_gc);
  }
  XCloseDisplay(g_dpy);
  BbcSessionFinish();
  return 0;
}
