//
//  BBC Micro emulator window for Windows. GDI draws the frame and waveOut
//  plays the sound. Both are part of every Windows install, so bbc.exe
//  needs no DLLs beyond the system's own.
//

#include <windows.h>
#include <mmsystem.h>

#include "bbc_session.h"
#include "bbc_hardware.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static HWND g_win = NULL;
static bool g_running = true;
static int g_fire0 = 0;
static int g_fire1 = 0;
static int g_client_w = 640;
static int g_client_h = 512;
static int g_sized_w = 0;
static int g_sized_h = 0;
static int g_aspect_w = 640;
static int g_aspect_h = 512;
static uint32_t g_pixels[BBC_FB_WIDTH * BBC_FB_HEIGHT];
static int g_pixels_w = 0;
static int g_pixels_h = 0;

#define AUDIO_BUFFERS 4
#define AUDIO_FRAMES 1024

static volatile LONG g_audio_run = 0;
static HANDLE g_audio_thread = NULL;
static HANDLE g_audio_event = NULL;
static HWAVEOUT g_wave = NULL;
static WAVEHDR g_wave_headers[AUDIO_BUFFERS];
static int16_t g_wave_samples[AUDIO_BUFFERS][AUDIO_FRAMES];

static uint64_t MonoNs(void) {
  static LARGE_INTEGER freq;
  LARGE_INTEGER now;
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
  }
  QueryPerformanceCounter(&now);
  return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000000ull +
         (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000000ull / (uint64_t)freq.QuadPart;
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

static bool LetterToBbc(UINT vk, int* column, int* row) {
  static const int cells[26][2] = {
      {1, 4}, {4, 6}, {2, 5}, {2, 3}, {2, 2}, {3, 4}, {3, 5}, {4, 5}, {5, 2}, {5, 4},
      {6, 4}, {6, 5}, {5, 6}, {5, 5}, {6, 3}, {7, 3}, {0, 1}, {3, 3}, {1, 5}, {3, 2},
      {5, 3}, {3, 6}, {1, 2}, {2, 4}, {4, 4}, {1, 6},
  };
  if (vk < 'A' || vk > 'Z') {
    return false;
  }
  *column = cells[vk - 'A'][0];
  *row = cells[vk - 'A'][1];
  return true;
}

static bool FunctionToBbc(UINT vk, int* column, int* row) {
  static const int cells[10][2] = {
      {1, 7}, {2, 7}, {3, 7}, {4, 1}, {4, 7}, {5, 7}, {6, 1}, {6, 7}, {7, 7}, {0, 2},
  };
  if (vk < VK_F1 || vk > VK_F10) {
    return false;
  }
  *column = cells[vk - VK_F1][0];
  *row = cells[vk - VK_F1][1];
  return true;
}

// With Num Lock off the keypad sends the navigation keys. Those arrive
// without the extended flag, which tells them from the separate cursor
// block. Keypad Enter is Return with the extended flag.
static bool KeypadToBbc(UINT vk, bool extended, int* column, int* row) {
  struct {
    UINT vk;
    UINT nav;
    int column;
    int row;
  } keys[] = {
      {VK_NUMPAD0, VK_INSERT, 10, 6}, {VK_NUMPAD1, VK_END, 11, 6},
      {VK_NUMPAD2, VK_DOWN, 12, 7},   {VK_NUMPAD3, VK_NEXT, 12, 6},
      {VK_NUMPAD4, VK_LEFT, 10, 7},   {VK_NUMPAD5, VK_CLEAR, 11, 7},
      {VK_NUMPAD6, VK_RIGHT, 10, 1},  {VK_NUMPAD7, VK_HOME, 11, 1},
      {VK_NUMPAD8, VK_UP, 10, 2},     {VK_NUMPAD9, VK_PRIOR, 11, 2},
      {VK_ADD, 0, 10, 3},             {VK_SUBTRACT, 0, 11, 3},
      {VK_DIVIDE, 0, 10, 4},          {VK_DECIMAL, VK_DELETE, 12, 4},
      {VK_MULTIPLY, 0, 11, 5},        {VK_OEM_NEC_EQUAL, 0, 10, 5},
  };
  size_t i;
  if (vk == VK_RETURN && extended) {
    *column = 12;
    *row = 3;
    return true;
  }
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (keys[i].vk == vk || (!extended && keys[i].nav != 0 && keys[i].nav == vk)) {
      *column = keys[i].column;
      *row = keys[i].row;
      return true;
    }
  }
  return false;
}

static bool SpecialToBbc(UINT vk, int* column, int* row) {
  struct {
    UINT vk;
    int column;
    int row;
  } keys[] = {
      {VK_ESCAPE, 0, 7}, {VK_TAB, 0, 6},   {VK_RETURN, 9, 4}, {VK_BACK, 9, 5},
      {VK_DELETE, 9, 5}, {VK_END, 9, 6},   {VK_LEFT, 9, 1},   {VK_RIGHT, 9, 7},
      {VK_DOWN, 9, 2},   {VK_UP, 9, 3},
  };
  size_t i;
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (keys[i].vk == vk) {
      *column = keys[i].column;
      *row = keys[i].row;
      return true;
    }
  }
  return false;
}

static bool ModifierKey(UINT vk) {
  return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL ||
         vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU ||
         vk == VK_RMENU || vk == VK_CAPITAL || vk == VK_LWIN || vk == VK_RWIN;
}

// AltGr arrives as Ctrl+Alt. It chooses a character, so it is not CTRL.
static bool ControlHeld(void) {
  return (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
}

// The scan code and extended flag name the physical key, so the release
// finds the press even if Shift changed in between.
static unsigned KeyCode(LPARAM lparam) {
  return (unsigned)((lparam >> 16) & 0x1ff);
}

static void PressHostKey(UINT vk, LPARAM lparam) {
  unsigned code = KeyCode(lparam);
  bool extended = (lparam & (1 << 24)) != 0;
  int column = 0;
  int row = 0;
  int shift_force = -1;
  bool mapped = false;
  bool control = ControlHeld();
  int slot;
  if (vk == VK_F12) {
    W65C02InterpreterResetCpu(&g_cpu);
    return;
  }
  if (ModifierKey(vk)) {
    return;
  }
  ReleaseHostKey(code);
  // The per-frame poll can lag a fast Shift+key, which would put Shift on ':'.
  g_host_shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  if (FunctionToBbc(vk, &column, &row)) {
    mapped = true;
  }
  if (!mapped && g_cpu.bbc != NULL && BbcMachineIsMaster(g_cpu.bbc) &&
      KeypadToBbc(vk, extended, &column, &row)) {
    mapped = true;
  }
  if (!mapped && SpecialToBbc(vk, &column, &row)) {
    mapped = true;
  }
  if (!mapped && LetterToBbc(vk, &column, &row)) {
    mapped = true;
  }
  if (!mapped && !control) {
    BYTE state[256];
    WCHAR text[4];
    bool need_shift = false;
    unsigned ch = 0;
    int n = 0;
    if (GetKeyboardState(state)) {
      n = ToUnicode(vk, (lparam >> 16) & 0xff, state, text, 4, 0);
    }
    if (n == 1) {
      ch = text[0];
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
  g_held[slot].code = code;
  g_held[slot].column = column;
  g_held[slot].row = row;
  g_held[slot].shift_force = shift_force;
  BbcMachineSetKey(g_cpu.bbc, column, row, true);
  SyncShift();
}

static void OnKey(UINT vk, LPARAM lparam, bool down) {
  unsigned code = KeyCode(lparam);
  int i;
  if (g_cpu.bbc == NULL) {
    return;
  }
  if (!down) {
    ReleaseHostKey(code);
    return;
  }
  // Bit 30 is set on auto-repeat.
  if ((lparam & (1 << 30)) != 0) {
    return;
  }
  for (i = 0; i < 16; i++) {
    if (g_held[i].used && g_held[i].code == code) {
      return;
    }
  }
  PressHostKey(vk, lparam);
}

static void DrawFrame(HDC dc) {
  BITMAPINFO bmi;
  if (g_pixels_w <= 0 || g_pixels_h <= 0) {
    RECT all = {0, 0, g_client_w, g_client_h};
    FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    return;
  }
  memset(&bmi, 0, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = g_pixels_w;
  // Negative height stores the rows top to bottom, as the framebuffer does.
  bmi.bmiHeader.biHeight = -g_pixels_h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(dc, COLORONCOLOR);
  StretchDIBits(dc, 0, 0, g_client_w, g_client_h, 0, 0, g_pixels_w, g_pixels_h, g_pixels, &bmi,
                DIB_RGB_COLORS, SRCCOPY);
}

static void BlitFrame(void) {
  int width;
  int height;
  const uint8_t* src;
  int x;
  int y;
  HDC dc;
  if (g_cpu.bbc == NULL) {
    return;
  }
  if (g_cpu.turbo || BbcMachineCompletedFrames(g_cpu.bbc) == 0) {
    BbcMachineRender(g_cpu.bbc);
  }
  width = BbcFrameWidth(g_cpu.bbc);
  height = BbcFrameHeight(g_cpu.bbc);
  if (width <= 0 || height <= 0 || width > BBC_FB_WIDTH || height > BBC_FB_HEIGHT) {
    return;
  }
  src = BbcFramebuffer(g_cpu.bbc);
  for (y = 0; y < height; y++) {
    const uint8_t* p = src + (size_t)y * BBC_FB_WIDTH * 3;
    uint32_t* out = g_pixels + (size_t)y * (size_t)width;
    for (x = 0; x < width; x++, p += 3) {
      out[x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
    }
  }
  g_pixels_w = width;
  g_pixels_h = height;
  dc = GetDC(g_win);
  if (dc != NULL) {
    DrawFrame(dc);
    ReleaseDC(g_win, dc);
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
  SetWindowTextA(g_win, title);
  shown = stamp;
}

static void FrameExtra(int* extra_w, int* extra_h) {
  RECT r = {0, 0, 0, 0};
  AdjustWindowRect(&r, (DWORD)GetWindowLongA(g_win, GWL_STYLE), FALSE);
  *extra_w = r.right - r.left;
  *extra_h = r.bottom - r.top;
}

static void FitWindow(int width, int height) {
  int want_h;
  int extra_w;
  int extra_h;
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
  g_aspect_w = width;
  g_aspect_h = want_h;
  if (IsZoomed(g_win)) {
    return;
  }
  FrameExtra(&extra_w, &extra_h);
  SetWindowPos(g_win, NULL, 0, 0, width + extra_w, want_h + extra_h,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Dragging an edge keeps the frame's shape, as the X11 aspect hint does.
static void KeepAspect(WPARAM edge, RECT* r) {
  int extra_w;
  int extra_h;
  int cw;
  int ch;
  FrameExtra(&extra_w, &extra_h);
  cw = r->right - r->left - extra_w;
  ch = r->bottom - r->top - extra_h;
  if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
    cw = ch * g_aspect_w / g_aspect_h;
    r->right = r->left + cw + extra_w;
    return;
  }
  ch = cw * g_aspect_h / g_aspect_w;
  if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
    r->top = r->bottom - ch - extra_h;
  } else {
    r->bottom = r->top + ch + extra_h;
  }
}

static void ReadHostInput(void) {
  POINT p;
  if (g_cpu.bbc == NULL) {
    return;
  }
  if (GetForegroundWindow() == g_win) {
    g_host_shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    SyncShift();
    BbcMachineSetKey(g_cpu.bbc, 1, 0, ControlHeld());
    BbcMachineSetKey(g_cpu.bbc, 0, 4, (GetKeyState(VK_CAPITAL) & 1) != 0);
  }
  if (GetCursorPos(&p) && ScreenToClient(g_win, &p) && p.x >= 0 && p.y >= 0 &&
      p.x < g_client_w && p.y < g_client_h && g_client_w > 1 && g_client_h > 1) {
    BbcMachineSetAnalogue(g_cpu.bbc, 0, (int)(p.x * 65535 / (g_client_w - 1)));
    BbcMachineSetAnalogue(g_cpu.bbc, 1, (int)(p.y * 65535 / (g_client_h - 1)));
  } else {
    BbcMachineSetAnalogue(g_cpu.bbc, 0, 0x8000);
    BbcMachineSetAnalogue(g_cpu.bbc, 1, 0x8000);
  }
  BbcMachineSetFire(g_cpu.bbc, 0, g_fire0 != 0);
  BbcMachineSetFire(g_cpu.bbc, 1, g_fire1 != 0);
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
  ReadHostInput();
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
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_CLOSE:
      g_running = false;
      return 0;
    case WM_SIZE:
      g_client_w = LOWORD(lparam);
      g_client_h = HIWORD(lparam);
      return 0;
    case WM_SIZING:
      KeepAspect(wparam, (RECT*)lparam);
      return TRUE;
    case WM_GETMINMAXINFO: {
      MINMAXINFO* info = (MINMAXINFO*)lparam;
      RECT r = {0, 0, 400, 320};
      AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
      info->ptMinTrackSize.x = r.right - r.left;
      info->ptMinTrackSize.y = r.bottom - r.top;
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      DrawFrame(dc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_KILLFOCUS:
      ReleaseAllKeys();
      return 0;
    case WM_KEYDOWN:
      OnKey((UINT)wparam, lparam, true);
      return 0;
    case WM_KEYUP:
      OnKey((UINT)wparam, lparam, false);
      return 0;
    // F10 and Alt combinations arrive as system keys. Taking them here
    // keeps F10 from opening the window menu. Alt+F4 still closes.
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      if (wparam == VK_F4) {
        break;
      }
      OnKey((UINT)wparam, lparam, msg == WM_SYSKEYDOWN);
      return 0;
    case WM_SYSCHAR:
      return 0;
    case WM_LBUTTONDOWN:
      g_fire0 = 1;
      SetCapture(hwnd);
      return 0;
    case WM_LBUTTONUP:
      g_fire0 = 0;
      if (!g_fire1) {
        ReleaseCapture();
      }
      return 0;
    case WM_RBUTTONDOWN:
      g_fire1 = 1;
      SetCapture(hwnd);
      return 0;
    case WM_RBUTTONUP:
      g_fire1 = 0;
      if (!g_fire0) {
        ReleaseCapture();
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static void QueueAudio(int index) {
  int got = 0;
  if (g_ready && g_cpu.bbc != NULL) {
    got = BbcMachineReadAudio(g_cpu.bbc, g_wave_samples[index], AUDIO_FRAMES);
  }
  if (got < AUDIO_FRAMES) {
    memset(g_wave_samples[index] + got, 0, (size_t)(AUDIO_FRAMES - got) * sizeof(int16_t));
  }
  waveOutWrite(g_wave, &g_wave_headers[index], sizeof(WAVEHDR));
}

static DWORD WINAPI AudioThread(LPVOID unused) {
  (void)unused;
  while (g_audio_run) {
    int i;
    WaitForSingleObject(g_audio_event, 100);
    for (i = 0; i < AUDIO_BUFFERS && g_audio_run; i++) {
      if ((g_wave_headers[i].dwFlags & WHDR_DONE) != 0) {
        QueueAudio(i);
      }
    }
  }
  return 0;
}

static bool OpenWave(unsigned rate) {
  WAVEFORMATEX format;
  memset(&format, 0, sizeof(format));
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 1;
  format.nSamplesPerSec = rate;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 2;
  format.nAvgBytesPerSec = rate * 2;
  return waveOutOpen(&g_wave, WAVE_MAPPER, &format, (DWORD_PTR)g_audio_event, 0,
                     CALLBACK_EVENT) == MMSYSERR_NOERROR;
}

static void StartAudio(void) {
  int mhz = g_cpu.clock_mhz > 0 ? g_cpu.clock_mhz : 2;
  unsigned rate = (unsigned)(48000 * mhz / 2);
  int i;
  g_audio_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (g_audio_event == NULL) {
    return;
  }
  if (!OpenWave(rate) && (rate == 48000 || !OpenWave(48000))) {
    fprintf(stderr, "bbc: unable to open the sound device\n");
    CloseHandle(g_audio_event);
    g_audio_event = NULL;
    g_wave = NULL;
    return;
  }
  for (i = 0; i < AUDIO_BUFFERS; i++) {
    memset(&g_wave_headers[i], 0, sizeof(WAVEHDR));
    g_wave_headers[i].lpData = (LPSTR)g_wave_samples[i];
    g_wave_headers[i].dwBufferLength = AUDIO_FRAMES * sizeof(int16_t);
    waveOutPrepareHeader(g_wave, &g_wave_headers[i], sizeof(WAVEHDR));
  }
  for (i = 0; i < AUDIO_BUFFERS; i++) {
    QueueAudio(i);
  }
  g_audio_run = 1;
  g_audio_thread = CreateThread(NULL, 0, AudioThread, NULL, 0, NULL);
  if (g_audio_thread == NULL) {
    g_audio_run = 0;
  }
}

static void StopAudio(void) {
  int i;
  if (g_wave == NULL) {
    return;
  }
  if (g_audio_thread != NULL) {
    g_audio_run = 0;
    SetEvent(g_audio_event);
    WaitForSingleObject(g_audio_thread, INFINITE);
    CloseHandle(g_audio_thread);
    g_audio_thread = NULL;
  }
  waveOutReset(g_wave);
  for (i = 0; i < AUDIO_BUFFERS; i++) {
    waveOutUnprepareHeader(g_wave, &g_wave_headers[i], sizeof(WAVEHDR));
  }
  waveOutClose(g_wave);
  g_wave = NULL;
  CloseHandle(g_audio_event);
  g_audio_event = NULL;
}

static bool OpenWindow(void) {
  WNDCLASSA wc;
  HINSTANCE instance = GetModuleHandleA(NULL);
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT r = {0, 0, 640, 512};
  memset(&wc, 0, sizeof(wc));
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpszClassName = "BbcMicro";
  if (!RegisterClassA(&wc)) {
    return false;
  }
  AdjustWindowRect(&r, style, FALSE);
  g_win = CreateWindowA("BbcMicro", "BBC Micro", style, CW_USEDEFAULT, CW_USEDEFAULT,
                        r.right - r.left, r.bottom - r.top, NULL, NULL, instance, NULL);
  if (g_win == NULL) {
    return false;
  }
  ShowWindow(g_win, SW_SHOWNORMAL);
  UpdateWindow(g_win);
  return true;
}

// bbc.exe is a GUI program, so it has no console of its own. Started from
// a command prompt, the ROM and socket messages go to that prompt. Output
// already redirected to a file or pipe is left there.
static void AttachParentConsole(void) {
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err != NULL && err != INVALID_HANDLE_VALUE) {
    return;
  }
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
    return;
  }
  freopen("CONOUT$", "w", stderr);
  freopen("CONOUT$", "w", stdout);
}

int main(int argc, char** argv) {
  int status = 1;
  uint64_t next;
  AttachParentConsole();
  if (!BbcSessionStart(argc, argv, &status)) {
    return status;
  }
  if (!OpenWindow()) {
    fprintf(stderr, "bbc: unable to open the window\n");
    BbcSessionFinish();
    return 1;
  }
  StartAudio();
  // Sleep in 1 ms steps, so a 20 ms frame is not rounded up to 31 ms.
  timeBeginPeriod(1);
  next = MonoNs();
  while (g_running) {
    MSG msg;
    // Keys are translated with ToUnicode as they are pressed. There is no
    // TranslateMessage, which would consume a dead key a second time.
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        g_running = false;
        break;
      }
      DispatchMessageA(&msg);
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
        DWORD wait = (DWORD)((next - now) / 1000000ull);
        MsgWaitForMultipleObjects(0, NULL, FALSE, wait, QS_ALLINPUT);
      }
    }
  }
  timeEndPeriod(1);
  StopAudio();
  DestroyWindow(g_win);
  BbcSessionFinish();
  return 0;
}
