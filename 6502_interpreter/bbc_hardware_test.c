#include "bbc_hardware.h"
#include "bbc_platform.h"
#include "cassette.h"
#include "cumana.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#endif

static int g_failures = 0;

// Scratch files go in the system temporary directory.
static const char* TempPath(char* buf, size_t n, const char* name) {
#ifdef _WIN32
  char dir[MAX_PATH];
  DWORD len = GetTempPathA(sizeof(dir), dir);
  if (len == 0 || len >= sizeof(dir)) {
    strcpy(dir, ".\\");
  }
  snprintf(buf, n, "%s%s", dir, name);
#else
  snprintf(buf, n, "/tmp/%s", name);
#endif
  return buf;
}

static char* MakeTempDir(char* templ) {
#ifdef _WIN32
  return _mktemp(templ) != NULL && _mkdir(templ) == 0 ? templ : NULL;
#else
  return mkdtemp(templ);
#endif
}

#define EXPECT(cond)                                                          \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      g_failures++;                                                           \
    }                                                                         \
  } while (0)

static int RgbEq(const uint8_t rgb[3], int r, int g, int b) {
  return rgb[0] == r && rgb[1] == g && rgb[2] == b;
}

static void TestFredJim(BbcMachine* bbc) {
  EXPECT(BbcMachineRead(bbc, 0xfc00) == 0xff);
  BbcMachineWrite(bbc, 0xfc10, 0x5a);
  EXPECT(BbcMachineRead(bbc, 0xfc10) == 0xff);
  EXPECT(BbcMachineRead(bbc, 0xfd00) == 0xff);
  BbcMachineWrite(bbc, 0xfcff, 1);
  BbcMachineWrite(bbc, 0xfd00, 0x42);
  BbcMachineWrite(bbc, 0xfcff, 0);
  BbcMachineWrite(bbc, 0xfd00, 0x11);
  BbcMachineWrite(bbc, 0xfcff, 1);
  EXPECT(BbcMachineRead(bbc, 0xfd00) == 0x42);
  EXPECT(BbcMachineRead(bbc, 0xfcff) == 1);
  BbcMachineWrite(bbc, 0xfcff, 0);
  EXPECT(BbcMachineRead(bbc, 0xfd00) == 0x11);
}

static void TestSheila(BbcMachine* bbc, uint8_t* ram) {
  uint8_t rgb[3];
  uint8_t rom[16];
  int x, y;
  int found = 0;

  BbcMachineWrite(bbc, 0xfe00, 12);
  BbcMachineWrite(bbc, 0xfe01, 0x06);
  EXPECT(BbcMachineRead(bbc, 0xfe03) == 0x06);
  EXPECT(BbcMachineRead(bbc, 0xfe20) == 0xfe);

  BbcMachineWrite(bbc, 0xfe10, 0xab);
  EXPECT(BbcMachineRead(bbc, 0xfe11) == 0xab);

  BbcMachineWrite(bbc, 0xfe63, 0xff);
  BbcMachineWrite(bbc, 0xfe61, 0x3c);
  EXPECT(BbcMachineRead(bbc, 0xfe71) == 0x3c);

  BbcMachineSelectMode(bbc, 0);
  EXPECT(BbcVideoAddress(bbc, 0x0600, 0) == 0x3000);
  EXPECT(BbcVideoAddress(bbc, 0x0600, 1) == 0x3001);
  EXPECT(BbcVideoAddress(bbc, 0x0601, 0) == 0x3008);
  ram[0x3000] = 0xff;
  BbcMachineRender(bbc);
  EXPECT(BbcFrameWidth(bbc) == 640);
  EXPECT(BbcFrameHeight(bbc) == 256);
  BbcPixel(bbc, 0, 0, rgb);
  EXPECT(RgbEq(rgb, 255, 255, 255));
  BbcPixel(bbc, 0, 1, rgb);
  EXPECT(RgbEq(rgb, 0, 0, 0));
  ram[0x3000] = 0x80;
  BbcMachineRender(bbc);
  BbcPixel(bbc, 0, 0, rgb);
  EXPECT(RgbEq(rgb, 255, 255, 255));
  BbcPixel(bbc, 1, 0, rgb);
  EXPECT(RgbEq(rgb, 0, 0, 0));

  BbcMachineSelectMode(bbc, 4);
  EXPECT(BbcVideoAddress(bbc, 0x0b00, 0) == 0x5800);
  BbcMachineRender(bbc);
  EXPECT(BbcFrameWidth(bbc) == 640);
  BbcMachineWrite(bbc, 0xfe22, 0x9c);
  BbcMachineRender(bbc);
  EXPECT(BbcFrameWidth(bbc) == 320);

  EXPECT(BbcVideoAddress(bbc, 0x1000, 0) == 0x4000);
  BbcMachineWrite(bbc, 0xfe40, 0x0c);
  EXPECT(BbcVideoAddress(bbc, 0x1000, 0) == 0x6000);

  BbcMachineSelectMode(bbc, 7);
  EXPECT(BbcVideoAddress(bbc, 0x7c00, 5) == 0x7c00);
  ram[0x7c00] = 'A';
  BbcMachineRender(bbc);
  EXPECT(BbcFrameWidth(bbc) == 640);
  EXPECT(BbcFrameHeight(bbc) == 500);
  for (y = 0; y < 10 && !found; y++) {
    for (x = 0; x < 16; x++) {
      BbcPixel(bbc, x, y, rgb);
      if (rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255) {
        found = 1;
        break;
      }
    }
  }
  EXPECT(found);

  // MODE 7 boots with a flashing underline. R10 mode 0 keeps it on, on the
  // bottom scanline of the cell at the cursor address. Each character row
  // is two framebuffer lines, so that underline is the last pair.
  BbcPixel(bbc, 0, 18, rgb);
  EXPECT(RgbEq(rgb, 0, 0, 0));
  BbcMachineWrite(bbc, 0xfe00, 14);
  BbcMachineWrite(bbc, 0xfe01, 0x7c);
  BbcMachineWrite(bbc, 0xfe00, 15);
  BbcMachineWrite(bbc, 0xfe01, 0x00);
  BbcMachineWrite(bbc, 0xfe00, 10);
  BbcMachineWrite(bbc, 0xfe01, 0x12);
  BbcMachineRender(bbc);
  BbcPixel(bbc, 0, 18, rgb);
  EXPECT(RgbEq(rgb, 255, 255, 255));

  memset(rom, 0x11, sizeof(rom));
  rom[0] = 0xab;
  EXPECT(BbcMachineLoadSidewaysBytes(bbc, 0, rom, sizeof(rom)));
  EXPECT(ram[0x8000] == 0xab);
  BbcMachineWrite(bbc, 0xfe30, 1);
  EXPECT(ram[0x8000] == 0xff);
  BbcMachineWrite(bbc, 0xfe32, 0);
  EXPECT(ram[0x8000] == 0xab);
  // The Model B mirrors ROMSEL across &FE30-&FE3F, including &FE34.
  BbcMachineWrite(bbc, 0xfe34, 1);
  EXPECT(ram[0x8000] == 0xff);
  BbcMachineWrite(bbc, 0xfe34, 0);
  EXPECT(ram[0x8000] == 0xab);

  BbcMachineWrite(bbc, 0xfe44, 0x0f);
  BbcMachineWrite(bbc, 0xfe45, 0x00);
  // Latch &000F is 16 VIA ticks, and the VIA counts at 1 MHz (32 CPU cycles).
  BbcMachineAdvance(bbc, 32);
  BbcMachineWrite(bbc, 0xfe4e, 0xc0);
  EXPECT(BbcMachineIrqPending(bbc));
  BbcMachineWrite(bbc, 0xfe4d, 0x40);
  EXPECT(!BbcMachineIrqPending(bbc));
  BbcMachineAdvance(bbc, 40000);
  EXPECT(!BbcMachineIrqPending(bbc));
  BbcMachineWrite(bbc, 0xfe4e, 0x82);
  EXPECT(BbcMachineIrqPending(bbc));
  EXPECT((BbcMachineRead(bbc, 0xfe4d) & 0x02) != 0);
  (void)BbcMachineRead(bbc, 0xfe41);
  EXPECT(!BbcMachineIrqPending(bbc));
}

static void TestOsAndPpm(BbcMachine* bbc, uint8_t* ram) {
  char os_buf[512];
  char ppm_buf[512];
  const char* os_path = TempPath(os_buf, sizeof(os_buf), "bbc-test-os.rom");
  const char* ppm_path = TempPath(ppm_buf, sizeof(ppm_buf), "bbc-test-screen.ppm");
  FILE* fp = fopen(os_path, "wb");
  uint8_t bytes[4] = {0x60, 0xea, 0xea, 0xea};
  char header[32];
  EXPECT(fp != NULL);
  if (fp != NULL) {
    EXPECT(fwrite(bytes, 1, sizeof(bytes), fp) == sizeof(bytes));
    fclose(fp);
  }
  EXPECT(BbcMachineLoadOs(bbc, os_path));
  EXPECT(ram[0xc000] == 0x60);
  BbcMachineSelectMode(bbc, 0);
  ram[0x3000] = 0xff;
  EXPECT(BbcMachineWritePpm(bbc, ppm_path));
  fp = fopen(ppm_path, "rb");
  EXPECT(fp != NULL);
  if (fp != NULL) {
    EXPECT(fgets(header, sizeof(header), fp) != NULL);
    EXPECT(strcmp(header, "P6\n") == 0);
    EXPECT(fgets(header, sizeof(header), fp) != NULL);
    EXPECT(strcmp(header, "640 256\n") == 0);
    fclose(fp);
  }
}

static void WriteCrtc(BbcMachine* bbc, int reg, int value) {
  BbcMachineWrite(bbc, 0xfe00, (uint8_t)reg);
  BbcMachineWrite(bbc, 0xfe01, (uint8_t)value);
}

static void AdvanceUntilFrame(BbcMachine* bbc) {
  int seen = BbcMachineCompletedFrames(bbc);
  int guard = 0;
  while (BbcMachineCompletedFrames(bbc) == seen && guard < 80000) {
    BbcMachineAdvance(bbc, 128);
    guard += 128;
  }
}

static void ReadOrigin(BbcMachine* bbc, uint8_t rgb[3]) {
  BbcMachineRender(bbc);
  BbcPixel(bbc, 0, 0, rgb);
}

// Elite switches the Video ULA from mode 4 to mode 5 when a VIA timer
// expires, without reprogramming the 6845. The beam has to keep the top
// of the frame in the old pixel format.
static void TestUlaSplit(void) {
  uint8_t* ram = calloc(65536, 1);
  BbcMachine* bbc = BbcMachineCreate();
  uint8_t mode4[3];
  uint8_t mode5[3];
  uint8_t rgb[3];
  int y;
  int split = -1;
  int i;
  EXPECT(ram != NULL && bbc != NULL);
  if (ram == NULL || bbc == NULL) {
    BbcMachineDestroy(bbc);
    free(ram);
    return;
  }
  memset(ram, 0xff, 65536);
  BbcMachineSetRam(bbc, ram);
  BbcMachineSelectMode(bbc, 4);
  ReadOrigin(bbc, mode4);
  BbcMachineWrite(bbc, 0xfe20, 0xc4);
  for (i = 0; i < 16; i++) {
    BbcMachineWrite(bbc, 0xfe21, (uint8_t)((i << 4) | 0x06));
  }
  ReadOrigin(bbc, mode5);
  EXPECT(!RgbEq(mode4, mode5[0], mode5[1], mode5[2]));
  BbcMachineSelectMode(bbc, 4);
  BbcMachineAdvance(bbc, 4 * 8 * 128);
  BbcMachineWrite(bbc, 0xfe20, 0xc4);
  for (i = 0; i < 16; i++) {
    BbcMachineWrite(bbc, 0xfe21, (uint8_t)((i << 4) | 0x06));
  }
  AdvanceUntilFrame(bbc);
  EXPECT(BbcMachineCompletedFrames(bbc) == 1);
  EXPECT(BbcFrameHeight(bbc) > 64);
  for (y = 0; y < BbcFrameHeight(bbc); y++) {
    BbcPixel(bbc, 0, y, rgb);
    if (!RgbEq(rgb, mode4[0], mode4[1], mode4[2])) {
      split = y;
      break;
    }
  }
  EXPECT(split > 8);
  if (split > 8) {
    BbcPixel(bbc, 0, split, rgb);
    EXPECT(RgbEq(rgb, mode5[0], mode5[1], mode5[2]));
    BbcPixel(bbc, 0, BbcFrameHeight(bbc) - 1, rgb);
    EXPECT(RgbEq(rgb, mode5[0], mode5[1], mode5[2]));
  }
  BbcMachineDestroy(bbc);
  free(ram);
}

// Changing R4 so the vertical counter wraps reloads R12/R13 without a
// vsync. The picture keeps going down the screen from the new address.
static void TestAddressRupture(void) {
  uint8_t* ram = calloc(65536, 1);
  BbcMachine* bbc = BbcMachineCreate();
  uint8_t top[3];
  uint8_t bottom[3];
  uint8_t rgb[3];
  int y;
  int split = -1;
  EXPECT(ram != NULL && bbc != NULL);
  if (ram == NULL || bbc == NULL) {
    BbcMachineDestroy(bbc);
    free(ram);
    return;
  }
  BbcMachineSetRam(bbc, ram);
  memset(ram + 0x3c00, 0xff, 0x400);
  BbcMachineSelectMode(bbc, 4);
  ReadOrigin(bbc, top);
  WriteCrtc(bbc, 12, 0x30);
  WriteCrtc(bbc, 13, 0x00);
  ReadOrigin(bbc, bottom);
  EXPECT(!RgbEq(top, bottom[0], bottom[1], bottom[2]));
  BbcMachineSelectMode(bbc, 4);
  BbcMachineAdvance(bbc, 4 * 8 * 128);
  WriteCrtc(bbc, 12, 0x30);
  WriteCrtc(bbc, 13, 0x00);
  // R4 ends the frame on this row so the new start address loads, and R7
  // is inside that new frame so vsync publishes both halves.
  WriteCrtc(bbc, 4, 4);
  WriteCrtc(bbc, 7, 3);
  AdvanceUntilFrame(bbc);
  EXPECT(BbcMachineCompletedFrames(bbc) == 1);
  for (y = 0; y < BbcFrameHeight(bbc); y++) {
    BbcPixel(bbc, 0, y, rgb);
    if (!RgbEq(rgb, top[0], top[1], top[2])) {
      split = y;
      break;
    }
  }
  EXPECT(split > 8);
  if (split > 8) {
    BbcPixel(bbc, 0, split, rgb);
    EXPECT(RgbEq(rgb, bottom[0], bottom[1], bottom[2]));
  }
  BbcMachineDestroy(bbc);
  free(ram);
}

static void TestTeletextBeam(void) {
  uint8_t* ram = calloc(65536, 1);
  BbcMachine* bbc = BbcMachineCreate();
  uint8_t rgb[3];
  int x;
  int y;
  int found = 0;
  EXPECT(ram != NULL && bbc != NULL);
  if (ram == NULL || bbc == NULL) {
    BbcMachineDestroy(bbc);
    free(ram);
    return;
  }
  BbcMachineSetRam(bbc, ram);
  BbcMachineSelectMode(bbc, 7);
  ram[0x7c00] = 'A';
  AdvanceUntilFrame(bbc);
  EXPECT(BbcMachineCompletedFrames(bbc) == 1);
  EXPECT(BbcFrameWidth(bbc) == 640);
  EXPECT(BbcFrameHeight(bbc) > 400);
  EXPECT(BbcFrameHeight(bbc) < 520);
  for (y = 0; y < 24 && !found; y++) {
    for (x = 0; x < 32; x++) {
      BbcPixel(bbc, x, y, rgb);
      if (rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255) {
        found = 1;
      }
    }
  }
  EXPECT(found);
  BbcMachineDestroy(bbc);
  free(ram);
}

static void TestKeyboardAndSound(BbcMachine* bbc) {
  int16_t samples[2048];
  int i;
  int energy = 0;
  int count;
  BbcMachineSetKey(bbc, 0, 1, true);
  BbcMachineWrite(bbc, 0xfe43, 0x7f);
  BbcMachineWrite(bbc, 0xfe41, 0x10);
  EXPECT((BbcMachineRead(bbc, 0xfe41) & 0x80) != 0);
  BbcMachineSetKey(bbc, 0, 1, false);
  EXPECT((BbcMachineRead(bbc, 0xfe41) & 0x80) == 0);

  BbcMachineWrite(bbc, 0xfe43, 0xff);
  BbcMachineWrite(bbc, 0xfe42, 0x0f);
  {
    static const uint8_t kTone[] = {0x8e, 0x1d, 0x90, 0xbf, 0xdf, 0xff};
    for (i = 0; i < 6; i++) {
      BbcMachineWrite(bbc, 0xfe41, kTone[i]);
      BbcMachineWrite(bbc, 0xfe40, 0x08);
      BbcMachineWrite(bbc, 0xfe40, 0x00);
    }
  }
  BbcMachineAdvance(bbc, 80000);
  count = BbcMachineReadAudio(bbc, samples, 2048);
  EXPECT(count > 1000);
  for (i = 0; i < count; i++) {
    energy += samples[i] < 0 ? -samples[i] : samples[i];
  }
  EXPECT(energy > 100000);
}

static int WaitBits(BbcMachine* bbc, uint16_t addr, uint8_t mask, int spins) {
  int i;
  for (i = 0; i < spins; i++) {
    if (BbcMachineRead(bbc, addr) & mask) {
      return 1;
    }
    BbcMachineAdvance(bbc, 16);
  }
  return 0;
}

static int WaitClear(BbcMachine* bbc, uint16_t addr, uint8_t mask, int spins) {
  int i;
  for (i = 0; i < spins; i++) {
    if ((BbcMachineRead(bbc, addr) & mask) == 0) {
      return 1;
    }
    BbcMachineAdvance(bbc, 16);
  }
  return 0;
}

static void WriteImage(const char* path, const uint8_t* data, size_t length) {
  FILE* fp = fopen(path, "wb");
  EXPECT(fp != NULL);
  if (fp != NULL) {
    EXPECT(fwrite(data, 1, length, fp) == length);
    fclose(fp);
  }
}

static void TestDisc(void) {
  char ssd_buf[512];
  char locked_buf[512];
  char adf_buf[512];
  const char* ssd = TempPath(ssd_buf, sizeof(ssd_buf), "bbc-disc-test.ssd");
  const char* locked = TempPath(locked_buf, sizeof(locked_buf), "bbc-disc-locked.ssd");
  const char* adf = TempPath(adf_buf, sizeof(adf_buf), "bbc-disc-test.adf");
  uint8_t image[2560];
  uint8_t adf_image[4096];
  uint8_t back[2560];
  uint8_t rom[8] = {'A', 'D', 'F', 'S', 0, 0, 0, 0};
  BbcMachine* bbc;
  FILE* fp;
  int i;
  uint8_t status;
  uint8_t result;

  for (i = 0; i < 2560; i++) {
    image[i] = (uint8_t)(i < 256 ? i : 0xe5);
  }
  memset(adf_image, 0, sizeof(adf_image));
  adf_image[0] = 0x3c;
  WriteImage(ssd, image, sizeof(image));
  WriteImage(locked, image, sizeof(image));
  WriteImage(adf, adf_image, sizeof(adf_image));
  EXPECT(chmod(locked, 0444) == 0);

  bbc = BbcMachineCreate();
  EXPECT(bbc != NULL);
  EXPECT(BbcMachineLoadDisc(bbc, 0, ssd));
  BbcMachineWrite(bbc, 0xfe80, 0x35);
  BbcMachineWrite(bbc, 0xfe81, 0x0d);
  BbcMachineWrite(bbc, 0xfe81, 1);
  BbcMachineWrite(bbc, 0xfe81, 1);
  BbcMachineWrite(bbc, 0xfe81, 0x10);
  EXPECT(BbcMachineRead(bbc, 0xfe80) == 0);
  EXPECT(!BbcMachineNmiPending(bbc));

  BbcMachineWrite(bbc, 0xfe80, 0x53);
  BbcMachineWrite(bbc, 0xfe81, 0x00);
  BbcMachineWrite(bbc, 0xfe81, 0x00);
  BbcMachineWrite(bbc, 0xfe81, 0x21);
  for (i = 0; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe80, 0x04, 80));
    if (i == 0) {
      EXPECT(BbcMachineNmiPending(bbc));
    }
    EXPECT(BbcMachineRead(bbc, 0xfe84) == (uint8_t)i);
  }
  EXPECT(WaitBits(bbc, 0xfe80, 0x10, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe81) == 0);

  BbcMachineWrite(bbc, 0xfe80, 0x2c);
  status = BbcMachineRead(bbc, 0xfe80);
  result = BbcMachineRead(bbc, 0xfe81);
  EXPECT((status & 0x10) != 0);
  EXPECT((result & 0x86) == 0x86);

  BbcMachineWrite(bbc, 0xfe80, 0x69);
  BbcMachineWrite(bbc, 0xfe81, 5);
  EXPECT(WaitBits(bbc, 0xfe80, 0x10, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe81) == 0);
  BbcMachineWrite(bbc, 0xfe80, 0x3d);
  BbcMachineWrite(bbc, 0xfe81, 0x12);
  EXPECT((BbcMachineRead(bbc, 0xfe80) & 0x10) != 0);
  EXPECT(BbcMachineRead(bbc, 0xfe81) == 5);

  BbcMachineWrite(bbc, 0xfe80, 0x4b);
  BbcMachineWrite(bbc, 0xfe81, 0);
  BbcMachineWrite(bbc, 0xfe81, 1);
  BbcMachineWrite(bbc, 0xfe81, 0x21);
  for (i = 0; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe80, 0x04, 80));
    BbcMachineWrite(bbc, 0xfe84, 0x5a);
  }
  EXPECT(WaitBits(bbc, 0xfe80, 0x10, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe81) == 0);
  BbcMachineWrite(bbc, 0xfe80, 0x53);
  BbcMachineWrite(bbc, 0xfe81, 0);
  BbcMachineWrite(bbc, 0xfe81, 1);
  BbcMachineWrite(bbc, 0xfe81, 0x21);
  for (i = 0; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe80, 0x04, 80));
    EXPECT(BbcMachineRead(bbc, 0xfe84) == 0x5a);
  }
  EXPECT(WaitBits(bbc, 0xfe80, 0x10, 80));
  (void)BbcMachineRead(bbc, 0xfe81);
  fp = fopen(ssd, "rb");
  EXPECT(fp != NULL);
  if (fp != NULL) {
    EXPECT(fread(back, 1, sizeof(back), fp) == sizeof(back));
    fclose(fp);
    EXPECT(back[0] == 0);
    EXPECT(back[255] == 255);
    EXPECT(back[256] == 0x5a);
    EXPECT(back[511] == 0x5a);
  }

  BbcMachineWrite(bbc, 0xfe80, 0x93);
  BbcMachineWrite(bbc, 0xfe81, 0);
  BbcMachineWrite(bbc, 0xfe81, 0);
  BbcMachineWrite(bbc, 0xfe81, 0x21);
  EXPECT(WaitBits(bbc, 0xfe80, 0x10, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe81) == 0x10);

  BbcMachineSetFdc(bbc, BBC_FDC_1770);
  EXPECT(BbcMachineRead(bbc, 0xfe80) == 0xfe);
  BbcMachineWrite(bbc, 0xfe80, 0x21);
  EXPECT(BbcMachineRead(bbc, 0xfe80) == 0xfe);
  BbcMachineWrite(bbc, 0xfe85, 0);
  BbcMachineWrite(bbc, 0xfe86, 0);
  BbcMachineWrite(bbc, 0xfe84, 0x80);
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  EXPECT(BbcMachineNmiPending(bbc));
  EXPECT(BbcMachineRead(bbc, 0xfe87) == 0);
  for (i = 1; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
    EXPECT(BbcMachineRead(bbc, 0xfe87) == (uint8_t)i);
  }
  EXPECT(WaitClear(bbc, 0xfe84, 0x01, 80));
  status = BbcMachineRead(bbc, 0xfe84);
  EXPECT((status & 0x10) == 0);

  BbcMachineWrite(bbc, 0xfe85, 0);
  BbcMachineWrite(bbc, 0xfe86, 2);
  BbcMachineWrite(bbc, 0xfe84, 0xa0);
  for (i = 0; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
    BbcMachineWrite(bbc, 0xfe87, 0xa5);
  }
  EXPECT(WaitClear(bbc, 0xfe84, 0x01, 80));
  EXPECT((BbcMachineRead(bbc, 0xfe84) & 0x50) == 0);
  BbcMachineWrite(bbc, 0xfe86, 2);
  BbcMachineWrite(bbc, 0xfe84, 0x80);
  for (i = 0; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
    EXPECT(BbcMachineRead(bbc, 0xfe87) == 0xa5);
  }
  EXPECT(WaitClear(bbc, 0xfe84, 0x01, 80));
  fp = fopen(ssd, "rb");
  EXPECT(fp != NULL);
  if (fp != NULL) {
    EXPECT(fread(back, 1, sizeof(back), fp) == sizeof(back));
    fclose(fp);
    EXPECT(back[512] == 0xa5);
    EXPECT(back[767] == 0xa5);
  }
  BbcMachineDestroy(bbc);

  bbc = BbcMachineCreate();
  EXPECT(BbcMachineLoadDisc(bbc, 0, locked));
  BbcMachineWrite(bbc, 0xfe80, 0x4b);
  BbcMachineWrite(bbc, 0xfe81, 0);
  BbcMachineWrite(bbc, 0xfe81, 0);
  BbcMachineWrite(bbc, 0xfe81, 0x21);
  EXPECT(WaitBits(bbc, 0xfe80, 0x10, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe81) == 0x12);
  BbcMachineDestroy(bbc);

  bbc = BbcMachineCreate();
  EXPECT(BbcMachineLoadDisc(bbc, 0, adf));
  BbcMachineWrite(bbc, 0xfe80, 0x21);
  BbcMachineWrite(bbc, 0xfe85, 0);
  BbcMachineWrite(bbc, 0xfe86, 0);
  BbcMachineWrite(bbc, 0xfe84, 0x80);
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe87) == 0x3c);
  BbcMachineDestroy(bbc);

  bbc = BbcMachineCreate();
  EXPECT(BbcMachineLoadDisc(bbc, 0, ssd));
  EXPECT(BbcMachineLoadSidewaysBytes(bbc, 15, rom, sizeof(rom)));
  BbcMachineWrite(bbc, 0xfe30, 15);
  BbcMachineWrite(bbc, 0xfe80, 0x21);
  BbcMachineWrite(bbc, 0xfe85, 0);
  BbcMachineWrite(bbc, 0xfe86, 0);
  BbcMachineWrite(bbc, 0xfe84, 0x80);
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe87) == 0);
  BbcMachineDestroy(bbc);

  EXPECT(chmod(locked, 0644) == 0);
  remove(ssd);
  remove(locked);
  remove(adf);
}

static int CellWhite(BbcMachine* bbc, int col, int y) {
  uint8_t rgb[3];
  int x;
  for (x = 0; x < 16; x++) {
    BbcPixel(bbc, col * 16 + x, y, rgb);
    if (rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255) {
      return 1;
    }
  }
  return 0;
}

static void TestPeripherals(void) {
  char tape_buf[512];
  char disc_buf[512];
  const char* tape_path = TempPath(tape_buf, sizeof(tape_buf), "bbc-tape-test.bin");
  const char* disc_path = TempPath(disc_buf, sizeof(disc_buf), "bbc-format-test.ssd");
  uint8_t image[2560];
  uint8_t sync = 0x2a;
  BbcMachine* bbc;
  int i;

  bbc = BbcMachineCreate();
  EXPECT(bbc != NULL);
  BbcMachineSetFdc(bbc, BBC_FDC_1770);
  EXPECT((BbcMachineRead(bbc, 0xfe84) & 0x02) != 0);
  BbcMachineAdvance(bbc, 8000);
  EXPECT((BbcMachineRead(bbc, 0xfe84) & 0x02) == 0);

  BbcMachineWrite(bbc, 0xfee0, 0x81);
  EXPECT(BbcMachineRead(bbc, 0xfee0) == 0xfe);
  BbcMachineWrite(bbc, 0xfe18, 0x5a);
  EXPECT(BbcMachineRead(bbc, 0xfe18) == 0xfe);
  BbcMachineWrite(bbc, 0xfe10, 0xab);
  EXPECT(BbcMachineRead(bbc, 0xfe11) == 0xab);

  BbcMachineWrite(bbc, 0xfe08, 0x03);
  EXPECT((BbcMachineRead(bbc, 0xfe08) & 0x82) == 0x02);

  BbcMachineSetAnalogue(bbc, 0, 0x8000);
  BbcMachineWrite(bbc, 0xfec0, 0x08);
  EXPECT((BbcMachineRead(bbc, 0xfec0) & 0x40) == 0);
  BbcMachineAdvance(bbc, 20000);
  EXPECT((BbcMachineRead(bbc, 0xfec0) & 0x40) != 0);
  EXPECT(BbcMachineRead(bbc, 0xfec1) == 0x80);
  EXPECT((BbcMachineRead(bbc, 0xfe4d) & 0x10) != 0);
  BbcMachineWrite(bbc, 0xfec0, 0x00);
  BbcMachineAdvance(bbc, 8000);
  EXPECT(BbcMachineRead(bbc, 0xfec1) == 0x80);
  EXPECT(BbcMachineRead(bbc, 0xfec2) == 0);

  BbcMachineWrite(bbc, 0xfe42, 0x00);
  EXPECT((BbcMachineRead(bbc, 0xfe40) & 0xd0) == 0xd0);
  BbcMachineSetFire(bbc, 0, true);
  EXPECT((BbcMachineRead(bbc, 0xfe40) & 0x10) == 0);
  EXPECT((BbcMachineRead(bbc, 0xfe40) & 0xc0) == 0xc0);

  BbcMachineWrite(bbc, 0xfe63, 0xff);
  BbcMachineWrite(bbc, 0xfe61, 0x5a);
  BbcMachineWrite(bbc, 0xfe6c, 0x0e);
  BbcMachineWrite(bbc, 0xfe6c, 0x0c);
  EXPECT(BbcMachinePrinterLength(bbc) == 1);
  EXPECT(BbcMachinePrinterByte(bbc, 0) == 0x5a);
  BbcMachineAdvance(bbc, 160);
  EXPECT((BbcMachineRead(bbc, 0xfe6d) & 0x02) != 0);

  BbcMachineWrite(bbc, 0xfe6b, 0x18);
  BbcMachineWrite(bbc, 0xfe6a, 0xa5);
  BbcMachineAdvance(bbc, 16);
  EXPECT((BbcMachineRead(bbc, 0xfe6d) & 0x04) != 0);
  BbcMachineDestroy(bbc);

  WriteImage(tape_path, &sync, 1);
  bbc = BbcMachineCreate();
  EXPECT(BbcMachineLoadTape(bbc, tape_path));
  BbcMachineWrite(bbc, 0xfe08, 0x03);
  BbcMachineWrite(bbc, 0xfe08, 0x15);
  BbcMachineWrite(bbc, 0xfe10, 0xc0);
  BbcMachineAdvance(bbc, 1);
  for (i = 0; i < 32; i++) {
    EXPECT((BbcMachineRead(bbc, 0xfe08) & 0x05) == 0x05);
    EXPECT(BbcMachineRead(bbc, 0xfe09) == 0xaa);
    BbcMachineAdvance(bbc, 2000);
  }
  EXPECT((BbcMachineRead(bbc, 0xfe08) & 0x05) == 0x01);
  EXPECT(BbcMachineRead(bbc, 0xfe09) == 0x2a);
  BbcMachineDestroy(bbc);
  remove(tape_path);

  memset(image, 0xe5, sizeof(image));
  WriteImage(disc_path, image, sizeof(image));
  bbc = BbcMachineCreate();
  EXPECT(BbcMachineLoadDisc(bbc, 0, disc_path));
  BbcMachineSetFdc(bbc, BBC_FDC_1770);
  BbcMachineWrite(bbc, 0xfe80, 0x21);
  BbcMachineWrite(bbc, 0xfe85, 0);
  BbcMachineWrite(bbc, 0xfe84, 0xf0);
  for (i = 0; i < 3; i++) {
    EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
    BbcMachineWrite(bbc, 0xfe87, 0xf5);
  }
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  BbcMachineWrite(bbc, 0xfe87, 0xfe);
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  BbcMachineWrite(bbc, 0xfe87, 0x00);
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  BbcMachineWrite(bbc, 0xfe87, 0x00);
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  BbcMachineWrite(bbc, 0xfe87, 0x02);
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  BbcMachineWrite(bbc, 0xfe87, 0x01);
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  BbcMachineWrite(bbc, 0xfe87, 0xfb);
  for (i = 0; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
    BbcMachineWrite(bbc, 0xfe87, 0xa5);
  }
  BbcMachineWrite(bbc, 0xfe84, 0xd0);
  BbcMachineWrite(bbc, 0xfe85, 0);
  BbcMachineWrite(bbc, 0xfe86, 2);
  BbcMachineWrite(bbc, 0xfe84, 0x80);
  EXPECT(WaitBits(bbc, 0xfe84, 0x02, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe87) == 0xa5);
  BbcMachineDestroy(bbc);
  remove(disc_path);
}

static void TestTeletextControls(void) {
  uint8_t* ram = calloc(65536, 1);
  BbcMachine* bbc = BbcMachineCreate();
  uint8_t rgb[3];
  EXPECT(ram != NULL && bbc != NULL);
  if (ram == NULL || bbc == NULL) {
    BbcMachineDestroy(bbc);
    free(ram);
    return;
  }
  BbcMachineSetRam(bbc, ram);
  BbcMachineSelectMode(bbc, 7);
  ram[0x7c00] = 0x17;
  ram[0x7c01] = 0x21;
  AdvanceUntilFrame(bbc);
  BbcPixel(bbc, 16, 2, rgb);
  EXPECT(RgbEq(rgb, 255, 255, 255));
  BbcPixel(bbc, 16, 8, rgb);
  EXPECT(RgbEq(rgb, 0, 0, 0));

  ram[0x7c00] = 0x0d;
  ram[0x7c01] = 0x17;
  ram[0x7c02] = 0x21;
  AdvanceUntilFrame(bbc);
  BbcPixel(bbc, 32, 4, rgb);
  EXPECT(RgbEq(rgb, 255, 255, 255));

  memset(ram + 0x7c00, 0, 40);
  ram[0x7c00] = 0x17;
  ram[0x7c01] = 0x7f;
  ram[0x7c02] = 0x1e;
  ram[0x7c03] = 0x01;
  AdvanceUntilFrame(bbc);
  EXPECT(CellWhite(bbc, 3, 4));
  EXPECT(!CellWhite(bbc, 2, 4));

  memset(ram + 0x7c00, 0, 40);
  ram[0x7c00] = 0x17;
  ram[0x7c01] = 0x1a;
  ram[0x7c02] = 0x7f;
  AdvanceUntilFrame(bbc);
  BbcPixel(bbc, 32, 4, rgb);
  EXPECT(RgbEq(rgb, 0, 0, 0));
  BbcPixel(bbc, 34, 4, rgb);
  EXPECT(RgbEq(rgb, 255, 255, 255));

  memset(ram + 0x7c00, 0, 40);
  ram[0x7c00] = 0x08;
  ram[0x7c01] = 'A';
  {
    int start = BbcMachineCompletedFrames(bbc);
    int y;
    int hidden = 1;
    while (BbcMachineCompletedFrames(bbc) < start + 26) {
      AdvanceUntilFrame(bbc);
    }
    for (y = 0; y < 10; y++) {
      if (CellWhite(bbc, 1, y)) {
        hidden = 0;
      }
    }
    EXPECT(hidden);
  }
  BbcMachineDestroy(bbc);
  free(ram);
}

static void WriteNamed(const char* dir, const char* name, uint8_t marker) {
  char path[512];
  FILE* fp;
  uint8_t body[16];
  memset(body, 0xff, sizeof(body));
  body[0] = marker;
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  fp = fopen(path, "wb");
  EXPECT(fp != NULL);
  if (fp != NULL) {
    EXPECT(fwrite(body, 1, sizeof(body), fp) == sizeof(body));
    fclose(fp);
  }
}

static int FindSlot(const BbcRomFile* files, int count, int slot) {
  int i;
  for (i = 0; i < count; i++) {
    if (files[i].slot == slot) {
      return i;
    }
  }
  return -1;
}

static void TestRomDirectory(void) {
  char dir[512];
  char path[512];
  BbcRomFile files[BBC_ROM_IMAGE_MAX];
  int count;
  int index;
  uint8_t* ram;
  BbcMachine* bbc;
  bool skip[16];
  TempPath(dir, sizeof(dir), "bbc-roms-XXXXXX");
  EXPECT(MakeTempDir(dir) != NULL);
  WriteNamed(dir, "os-1.20.rom", 0x11);
  WriteNamed(dir, "15-basic2.rom", 0x22);
  WriteNamed(dir, "14-adfs-1.30.rom", 0x33);
  WriteNamed(dir, "4.rom", 0x44);
  WriteNamed(dir, "notes.txt", 0x55);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX, BBC_FS_ANY);
  EXPECT(count == 4);
  index = FindSlot(files, count, -1);
  EXPECT(index >= 0);
  EXPECT(index >= 0 && strstr(files[index].path, "os-1.20.rom") != NULL);
  EXPECT(FindSlot(files, count, 15) >= 0);
  EXPECT(FindSlot(files, count, 14) >= 0);
  EXPECT(FindSlot(files, count, 4) >= 0);
  ram = calloc(65536, 1);
  bbc = BbcMachineCreate();
  EXPECT(ram != NULL && bbc != NULL);
  if (ram != NULL && bbc != NULL) {
    memset(skip, 0, sizeof(skip));
    skip[15] = true;
    BbcMachineSetRam(bbc, ram);
    EXPECT(BbcMachineLoadRomFiles(bbc, files, count, skip));
    BbcMachineWrite(bbc, 0xfe30, 14);
    EXPECT(ram[0x8000] == 0x33);
    BbcMachineWrite(bbc, 0xfe30, 4);
    EXPECT(ram[0x8000] == 0x44);
    BbcMachineWrite(bbc, 0xfe30, 15);
    EXPECT(ram[0x8000] == 0xff);
  }
  BbcRomFileFree(files, count);
  BbcMachineDestroy(bbc);
  free(ram);

  WriteNamed(dir, "14-dfs-1.20.rom", 0x88);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX, BBC_FS_ANY);
  EXPECT(count == 5);
  {
    int dfs_slot = -1;
    int adfs_slot = -1;
    int n;
    for (n = 0; n < count; n++) {
      if (files[n].path != NULL && strstr(files[n].path, "14-dfs-1.20.rom") != NULL) {
        dfs_slot = files[n].slot;
      }
      if (files[n].path != NULL && strstr(files[n].path, "14-adfs-1.30.rom") != NULL) {
        adfs_slot = files[n].slot;
      }
    }
    EXPECT(dfs_slot >= 0 && adfs_slot >= 0 && dfs_slot != adfs_slot);
    EXPECT(dfs_slot == 14 || adfs_slot == 14);
    EXPECT(dfs_slot == 13 || adfs_slot == 13);
  }
  BbcRomFileFree(files, count);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX, BBC_FS_DFS);
  EXPECT(count == 4);
  index = FindSlot(files, count, 14);
  EXPECT(index >= 0 && strstr(files[index].path, "14-dfs-1.20.rom") != NULL);
  EXPECT(FindSlot(files, count, 15) >= 0);
  BbcRomFileFree(files, count);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX, BBC_FS_ADFS);
  EXPECT(count == 4);
  index = FindSlot(files, count, 14);
  EXPECT(index >= 0 && strstr(files[index].path, "14-adfs-1.30.rom") != NULL);
  BbcRomFileFree(files, count);
  snprintf(path, sizeof(path), "%s/14-dfs-1.20.rom", dir);
  remove(path);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX, BBC_FS_DFS);
  EXPECT(count < 0);
  EXPECT(BbcMachineParseFilingSystem("dfs") == BBC_FS_DFS);
  EXPECT(BbcMachineParseFilingSystem("disk") == BBC_FS_DFS);
  EXPECT(BbcMachineParseFilingSystem("ADFS") == BBC_FS_ADFS);
  EXPECT(BbcMachineParseFilingSystem("tape") < 0);
  EXPECT(BbcMachineRomFilingKind("14-adfs-1.30.rom") == BBC_FS_ADFS);
  EXPECT(BbcMachineRomFilingKind("/roms/11-dnfs-1.20.rom") == BBC_FS_DFS);
  EXPECT(BbcMachineRomFilingKind("15-basic2.rom") == BBC_FS_ANY);
  {
    BbcMachine* machine = BbcMachineCreate();
    uint8_t image[8];
    EXPECT(machine != NULL);
    EXPECT(BbcMachineControllerForFiling(machine, BBC_FS_DFS) == BBC_FDC_8271);
    EXPECT(BbcMachineControllerForFiling(machine, BBC_FS_ADFS) == BBC_FDC_1770);
    memcpy(image, "1770DFS", 7);
    EXPECT(BbcMachineLoadSidewaysBytes(machine, 3, image, 7));
    EXPECT(BbcMachineControllerForFiling(machine, BBC_FS_DFS) == BBC_FDC_1770);
    memcpy(image, "ADFS", 4);
    EXPECT(BbcMachineLoadSidewaysBytes(machine, 14, image, 4));
    memcpy(image, "DFS", 3);
    EXPECT(BbcMachineLoadSidewaysBytes(machine, 11, image, 3));
    BbcMachineWrite(machine, 0xfe30, 14);
    EXPECT(BbcMachineRead(machine, 0xfe80) == 0xfe);
    BbcMachineWrite(machine, 0xfe30, 11);
    EXPECT(BbcMachineRead(machine, 0xfe80) == 0x00);
    BbcMachineWrite(machine, 0xfe30, 15);
    EXPECT(BbcMachineRead(machine, 0xfe80) == 0x00);
    BbcMachineDestroy(machine);
  }

  WriteNamed(dir, "junk.rom", 0x66);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX, BBC_FS_ANY);
  EXPECT(count < 0);
  snprintf(path, sizeof(path), "%s/junk.rom", dir);
  remove(path);
  WriteNamed(dir, "15-other.rom", 0x77);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX, BBC_FS_ANY);
  EXPECT(count < 0);

  snprintf(path, sizeof(path), "%s/os-1.20.rom", dir);
  remove(path);
  snprintf(path, sizeof(path), "%s/15-basic2.rom", dir);
  remove(path);
  snprintf(path, sizeof(path), "%s/15-other.rom", dir);
  remove(path);
  snprintf(path, sizeof(path), "%s/14-adfs-1.30.rom", dir);
  remove(path);
  snprintf(path, sizeof(path), "%s/4.rom", dir);
  remove(path);
  snprintf(path, sizeof(path), "%s/notes.txt", dir);
  remove(path);
  rmdir(dir);
}

static void TestMaster(void) {
  uint8_t* ram = calloc(65536, 1);
  BbcMachine* bbc = BbcMachineCreate();
  uint8_t image[16];
  uint8_t os[32];
  uint8_t rgb[3];
  bool banks[16];
  bool before;
  FILE* fp;
  char os_buf[512];
  const char* os_path = TempPath(os_buf, sizeof(os_buf), "bbc-master-os.rom");
  int i;
  EXPECT(ram != NULL && bbc != NULL);
  EXPECT(BbcMachineParseModel("b") == BBC_MACHINE_B);
  EXPECT(BbcMachineParseModel("MASTER128") == BBC_MACHINE_MASTER);
  EXPECT(BbcMachineParseModel("master256") == BBC_MACHINE_MASTER256);
  EXPECT(BbcMachineParseModel("compact") < 0);
  EXPECT(BbcMachineParseSidewaysRam("4", banks));
  EXPECT(banks[4] && banks[7] && !banks[0] && !banks[3] && !banks[8]);
  EXPECT(BbcMachineParseSidewaysRam("8", banks));
  EXPECT(banks[0] && banks[7] && !banks[8]);
  EXPECT(BbcMachineParseSidewaysRam("16", banks));
  EXPECT(banks[15]);
  EXPECT(BbcMachineParseSidewaysRam("4-7", banks));
  EXPECT(banks[4] && banks[7] && !banks[3] && !banks[8]);
  EXPECT(BbcMachineParseSidewaysRam("1,15", banks));
  EXPECT(banks[1] && banks[15] && !banks[0] && !banks[2]);
  EXPECT(!BbcMachineParseSidewaysRam("nope", banks));
  EXPECT(strcmp(BbcMachineRomDirectoryName(BBC_MACHINE_B), "bbc_b_rom_sockets") == 0);
  EXPECT(strcmp(BbcMachineRomDirectoryName(BBC_MACHINE_MASTER256), "bbc_master_rom_sockets") == 0);
  if (ram == NULL || bbc == NULL) {
    BbcMachineDestroy(bbc);
    free(ram);
    return;
  }
  BbcMachineSetRam(bbc, ram);
  EXPECT(BbcMachineSetModel(bbc, BBC_MACHINE_MASTER));
  EXPECT(BbcMachineIsMaster(bbc));
  EXPECT(BbcMachineControllerForFiling(bbc, BBC_FS_DFS) == BBC_FDC_1770);
  EXPECT(BbcMachineControllerForFiling(bbc, BBC_FS_ADFS) == BBC_FDC_1770);

  BbcMachineWrite(bbc, 0xfe30, 4);
  ram[0x8000] = 0x42;
  ram[0x9000] = 0x22;
  BbcMachineWrite(bbc, 0xfe32, 5);
  EXPECT(BbcMachineRead(bbc, 0xfe30) == 5);
  EXPECT(ram[0x8000] == 0x00);
  BbcMachineWrite(bbc, 0xfe30, 4);
  EXPECT(ram[0x8000] == 0x42);
  EXPECT(ram[0x9000] == 0x22);

  memset(image, 0xab, sizeof(image));
  EXPECT(BbcMachineLoadSidewaysBytes(bbc, 4, image, sizeof(image)));
  EXPECT(ram[0x8000] == 0xab);
  ram[0x8000] = 0xcd;
  BbcMachineWrite(bbc, 0xfe30, 5);
  BbcMachineWrite(bbc, 0xfe30, 4);
  EXPECT(ram[0x8000] == 0xcd);

  memset(image, 0x11, sizeof(image));
  EXPECT(BbcMachineLoadSidewaysBytes(bbc, 15, image, sizeof(image)));
  BbcMachineWrite(bbc, 0xfe30, 15);
  EXPECT(ram[0x8000] == 0x11);
  EXPECT(BbcMachineSidewaysRom(bbc, 0x8000));
  EXPECT(!BbcMachineSidewaysRom(bbc, 0x7fff));
  BbcMachineWrite(bbc, 0xfe30, 4);
  EXPECT(!BbcMachineSidewaysRom(bbc, 0x8000));
  BbcMachineWrite(bbc, 0xfe30, 15);
  ram[0x8000] = 0x99;
  BbcMachineWrite(bbc, 0xfe30, 4);
  BbcMachineWrite(bbc, 0xfe30, 15);
  EXPECT(ram[0x8000] == 0x11);

  BbcMachineWrite(bbc, 0xfe30, 4);
  EXPECT(ram[0x8000] == 0xcd);
  ram[0x9000] = 0x22;
  BbcMachineWrite(bbc, 0xfe30, 0x84);
  EXPECT(BbcMachineRead(bbc, 0xfe30) == 0x84);
  EXPECT(ram[0x8000] == 0x00);
  EXPECT(ram[0x9000] == 0x22);
  ram[0x8000] = 0x33;
  ram[0x9000] = 0x44;
  BbcMachineWrite(bbc, 0xfe30, 0x85);
  EXPECT(ram[0x8000] == 0x33);
  EXPECT(ram[0x9000] == 0x00);
  BbcMachineWrite(bbc, 0xfe30, 0x84);
  EXPECT(ram[0x8000] == 0x33);
  EXPECT(ram[0x9000] == 0x44);
  BbcMachineWrite(bbc, 0xfe30, 4);
  EXPECT(ram[0x8000] == 0xcd);
  EXPECT(ram[0x9000] == 0x44);
  BbcMachineWrite(bbc, 0xfe30, 0x84);
  EXPECT(ram[0x8000] == 0x33);

  memset(os, 0, sizeof(os));
  os[0] = 0xa5;
  fp = fopen(os_path, "wb");
  EXPECT(fp != NULL);
  if (fp != NULL) {
    EXPECT(fwrite(os, 1, sizeof(os), fp) == sizeof(os));
    fclose(fp);
    EXPECT(BbcMachineLoadOs(bbc, os_path));
    remove(os_path);
  }
  EXPECT(ram[0xc000] == 0xa5);
  BbcMachineWrite(bbc, 0xfe34, 0x08);
  EXPECT(BbcMachineRead(bbc, 0xfe34) == 0x08);
  EXPECT(BbcMachineRead(bbc, 0xfe30) == 0x84);
  EXPECT(ram[0xc000] == 0x00);
  ram[0xc000] = 0x5a;
  ram[0xe000] = 0x11;
  BbcMachineWrite(bbc, 0xfe34, 0x00);
  EXPECT(ram[0xc000] == 0xa5);
  EXPECT(ram[0xe000] == 0x11);
  BbcMachineWrite(bbc, 0xfe34, 0x08);
  EXPECT(ram[0xc000] == 0x5a);
  BbcMachineWrite(bbc, 0xfe34, 0x00);

  ram[0x3000] = 0xaa;
  BbcMachineWrite(bbc, 0xfe34, 0x04);
  EXPECT(ram[0x3000] == 0x00);
  ram[0x3000] = 0xbb;
  BbcMachineWrite(bbc, 0xfe34, 0x00);
  EXPECT(ram[0x3000] == 0xaa);
  BbcMachineWrite(bbc, 0xfe34, 0x04);
  EXPECT(ram[0x3000] == 0xbb);
  BbcMachineWrite(bbc, 0xfe34, 0x00);
  ram[0x3100] = 0x11;
  BbcMachineWrite(bbc, 0xfe34, 0x02);
  EXPECT(ram[0x3100] == 0x11);
  BbcMachineBeginInstruction(bbc, 0xc123);
  EXPECT(ram[0x3100] == 0x00);
  ram[0x3100] = 0x22;
  BbcMachineBeginInstruction(bbc, 0xe000);
  EXPECT(ram[0x3100] == 0x11);
  BbcMachineBeginInstruction(bbc, 0xc123);
  EXPECT(ram[0x3100] == 0x22);
  BbcMachineWrite(bbc, 0xfe34, 0x06);
  BbcMachineBeginInstruction(bbc, 0x0100);
  EXPECT(ram[0x3100] == 0x22);
  BbcMachineWrite(bbc, 0xfe34, 0x00);
  EXPECT(ram[0x3100] == 0x11);

  BbcMachineWrite(bbc, 0xfe34, 0x04);
  ram[0x3000] = 0x00;
  BbcMachineWrite(bbc, 0xfe34, 0x00);
  ram[0x3000] = 0xff;
  BbcMachineSelectMode(bbc, 0);
  BbcMachineRender(bbc);
  BbcPixel(bbc, 0, 0, rgb);
  EXPECT(RgbEq(rgb, 255, 255, 255));
  BbcMachineWrite(bbc, 0xfe34, 0x01);
  BbcMachineRender(bbc);
  BbcPixel(bbc, 0, 0, rgb);
  EXPECT(RgbEq(rgb, 0, 0, 0));
  BbcMachineWrite(bbc, 0xfe34, 0x00);

  BbcMachineWrite(bbc, 0xfcff, 3);
  BbcMachineWrite(bbc, 0xfd10, 0x77);
  EXPECT(BbcMachineRead(bbc, 0xfd10) == 0x77);
  BbcMachineWrite(bbc, 0xfe34, 0x20);
  EXPECT(BbcMachineRead(bbc, 0xfd10) == 0xff);
  BbcMachineWrite(bbc, 0xfd10, 0x66);
  BbcMachineWrite(bbc, 0xfe34, 0x00);
  EXPECT(BbcMachineRead(bbc, 0xfd10) == 0x77);

  before = BbcMachineIrqPending(bbc);
  BbcMachineWrite(bbc, 0xfe34, 0x80);
  EXPECT(BbcMachineIrqPending(bbc));
  BbcMachineWrite(bbc, 0xfe34, 0x00);
  EXPECT(BbcMachineIrqPending(bbc) == before);

  BbcMachineWrite(bbc, 0xfe43, 0xff);
  BbcMachineWrite(bbc, 0xfe41, 0x0e);
  BbcMachineWrite(bbc, 0xfe40, 0xcb);
  BbcMachineWrite(bbc, 0xfe40, 0x4b);
  BbcMachineWrite(bbc, 0xfe40, 0x41);
  BbcMachineWrite(bbc, 0xfe41, 0x5a);
  BbcMachineWrite(bbc, 0xfe40, 0x4a);
  BbcMachineWrite(bbc, 0xfe40, 0x42);
  BbcMachineWrite(bbc, 0xfe40, 0x49);
  BbcMachineWrite(bbc, 0xfe40, 0x4a);
  BbcMachineWrite(bbc, 0xfe43, 0x00);
  EXPECT(BbcMachineRead(bbc, 0xfe41) == 0x5a);
  BbcMachineWrite(bbc, 0xfe43, 0xff);
  BbcMachineWrite(bbc, 0xfe41, 0x0d);
  BbcMachineWrite(bbc, 0xfe40, 0xcb);
  BbcMachineWrite(bbc, 0xfe40, 0x49);
  BbcMachineWrite(bbc, 0xfe40, 0x4a);
  BbcMachineWrite(bbc, 0xfe43, 0x00);
  EXPECT(BbcMachineRead(bbc, 0xfe41) == 0x80);
  BbcMachineWrite(bbc, 0xfe40, 0x03);

  EXPECT(BbcMachineSetModel(bbc, BBC_MACHINE_MASTER256));
  for (i = 0; i < 16; i++) {
    BbcMachineWrite(bbc, 0xfe30, (uint8_t)i);
    ram[0x8000] = (uint8_t)(0x10 + i);
  }
  for (i = 0; i < 16; i++) {
    BbcMachineWrite(bbc, 0xfe30, (uint8_t)i);
    EXPECT(ram[0x8000] == (uint8_t)(0x10 + i));
  }

  {
    uint8_t* image = malloc(16384);
    EXPECT(image != NULL);
    if (image != NULL) {
      memset(image, 0xee, 16384);
      image[0] = 0xa5;
      image[0x3c00] = 0x20;
      image[0x3e34] = 0x5a;
      image[0x3e42] = 0x61;
      fp = fopen(os_path, "wb");
      EXPECT(fp != NULL);
      if (fp != NULL) {
        EXPECT(fwrite(image, 1, 16384, fp) == 16384);
        fclose(fp);
        EXPECT(BbcMachineLoadOs(bbc, os_path));
        remove(os_path);
      }
      BbcMachineWrite(bbc, 0xfe34, 0x40);
      EXPECT(ram[0xfc00] == 0x20);
      EXPECT(BbcMachineRead(bbc, 0xfe34) == 0x5a);
      BbcMachineWrite(bbc, 0xfe42, 0xff);
      EXPECT(BbcMachineRead(bbc, 0xfe42) == 0x61);
      BbcMachineWrite(bbc, 0xfe34, 0x00);
      EXPECT(ram[0xfc00] == 0xff);
      EXPECT(BbcMachineRead(bbc, 0xfe34) == 0x00);
      EXPECT(BbcMachineRead(bbc, 0xfe42) == 0xff);
      free(image);
    }
  }
  BbcMachineWrite(bbc, 0xfe24, 0x04);
  BbcMachineWrite(bbc, 0xfe29, 0x2a);
  EXPECT(BbcMachineRead(bbc, 0xfe29) == 0x2a);
  BbcMachineWrite(bbc, 0xfe85, 0x11);
  EXPECT(BbcMachineRead(bbc, 0xfe29) == 0x2a);
  EXPECT(BbcMachineRead(bbc, 0xfe80) == 0xfe);

  BbcMachineDestroy(bbc);
  free(ram);
}

struct CumanaTrial {
  int port;
  int drive;
  const char* name;
  const uint8_t* image;
  size_t length;
  pthread_mutex_t mu;
  int ready;
  int failed;
  int assigned;
  uint8_t* written;
  size_t written_len;
};

static int CumanaConnect(int port) {
  int fd;
  int one = 1;
  struct sockaddr_in addr;
  if (!SocketStartup()) {
    return -1;
  }
  fd = (int)socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    SocketClose(fd);
    return -1;
  }
  return fd;
}

static void* CumanaRefuseMain(void* arg) {
  struct CumanaTrial* trial = (struct CumanaTrial*)arg;
  int fd;
  int code = CUMANA_OK;
  int assigned = -1;
  char reply[64];
  fd = CumanaConnect(trial->port);
  if (fd < 0 ||
      CumanaSendInsert(fd, trial->drive, 0, trial->name, trial->image, trial->length) != 0 ||
      CumanaReadReply(fd, &code, &assigned, reply, sizeof(reply)) != 0) {
    if (fd >= 0) {
      SocketClose(fd);
    }
    pthread_mutex_lock(&trial->mu);
    trial->failed = 1;
    trial->ready = 1;
    pthread_mutex_unlock(&trial->mu);
    return NULL;
  }
  SocketClose(fd);
  pthread_mutex_lock(&trial->mu);
  trial->failed = code != CUMANA_ERR;
  trial->ready = 1;
  pthread_mutex_unlock(&trial->mu);
  return NULL;
}

static void* CumanaTrialMain(void* arg) {
  struct CumanaTrial* trial = (struct CumanaTrial*)arg;
  int fd;
  int code = CUMANA_ERR;
  int assigned = -1;
  char reply[64];
  uint8_t* written = NULL;
  size_t written_len = 0;
  fd = CumanaConnect(trial->port);
  if (fd < 0 ||
      CumanaSendInsert(fd, trial->drive, 0, trial->name, trial->image, trial->length) != 0 ||
      CumanaReadReply(fd, &code, &assigned, reply, sizeof(reply)) != 0 || code != CUMANA_OK) {
    if (fd >= 0) {
      SocketClose(fd);
    }
    pthread_mutex_lock(&trial->mu);
    trial->failed = 1;
    trial->ready = 1;
    pthread_mutex_unlock(&trial->mu);
    return NULL;
  }
  pthread_mutex_lock(&trial->mu);
  trial->assigned = assigned;
  trial->ready = 1;
  pthread_mutex_unlock(&trial->mu);
  if (CumanaReadImage(fd, &written, &written_len) == 0) {
    pthread_mutex_lock(&trial->mu);
    trial->written = written;
    trial->written_len = written_len;
    pthread_mutex_unlock(&trial->mu);
  }
  for (;;) {
    uint8_t* extra = NULL;
    size_t extra_len = 0;
    if (CumanaReadImage(fd, &extra, &extra_len) != 0) {
      break;
    }
    free(extra);
  }
  SocketClose(fd);
  return NULL;
}

static int WaitTrial(BbcMachine* bbc, struct CumanaTrial* trial, int want_write) {
  int i;
  for (i = 0; i < 2000; i++) {
    int done;
    pthread_mutex_lock(&trial->mu);
    done = trial->failed || (trial->ready && (!want_write || trial->written != NULL));
    pthread_mutex_unlock(&trial->mu);
    if (done) {
      return 1;
    }
    if (bbc != NULL) {
      BbcMachineAdvance(bbc, 200);
    }
    usleep(1000);
  }
  return 0;
}

static void TestCumana(void) {
  uint8_t image[2560];
  uint8_t side[2560];
  uint8_t adl[512];
  struct CumanaTrial drive0;
  struct CumanaTrial drive2;
  struct CumanaTrial refused;
  pthread_t thread0;
  pthread_t thread2;
  pthread_t thread_refused;
  BbcMachine* bbc;
  int port;
  int i;
  memset(&drive0, 0, sizeof(drive0));
  memset(&drive2, 0, sizeof(drive2));
  memset(&refused, 0, sizeof(refused));
  for (i = 0; i < 2560; i++) {
    image[i] = (uint8_t)(i < 256 ? i : 0xe5);
    side[i] = 0x42;
  }
  memset(adl, 0x11, sizeof(adl));
  bbc = BbcMachineCreate();
  EXPECT(bbc != NULL);
  port = BbcMachineListenDiscs(bbc, 0);
  EXPECT(port > 0);
  pthread_mutex_init(&drive0.mu, NULL);
  pthread_mutex_init(&drive2.mu, NULL);
  drive0.port = port;
  drive0.drive = 0;
  drive0.name = "blank.ssd";
  drive0.image = image;
  drive0.length = sizeof(image);
  drive2.port = port;
  drive2.drive = 2;
  drive2.name = "side.ssd";
  drive2.image = side;
  drive2.length = sizeof(side);
  EXPECT(pthread_create(&thread0, NULL, CumanaTrialMain, &drive0) == 0);
  EXPECT(WaitTrial(bbc, &drive0, 0));
  EXPECT(!drive0.failed);
  EXPECT(drive0.assigned == 0);
  EXPECT(pthread_create(&thread2, NULL, CumanaTrialMain, &drive2) == 0);
  EXPECT(WaitTrial(bbc, &drive2, 0));
  EXPECT(!drive2.failed);
  EXPECT(drive2.assigned == 2);

  pthread_mutex_init(&refused.mu, NULL);
  refused.port = port;
  refused.drive = 2;
  refused.name = "both.adl";
  refused.image = adl;
  refused.length = sizeof(adl);
  EXPECT(pthread_create(&thread_refused, NULL, CumanaRefuseMain, &refused) == 0);
  EXPECT(WaitTrial(bbc, &refused, 0));
  EXPECT(!refused.failed);

  BbcMachineWrite(bbc, 0xfe80, 0x53);
  BbcMachineWrite(bbc, 0xfe81, 0x00);
  BbcMachineWrite(bbc, 0xfe81, 0x00);
  BbcMachineWrite(bbc, 0xfe81, 0x21);
  EXPECT(WaitBits(bbc, 0xfe80, 0x04, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe84) == 0);
  for (i = 1; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe80, 0x04, 80));
    EXPECT(BbcMachineRead(bbc, 0xfe84) == (uint8_t)i);
  }
  EXPECT(WaitBits(bbc, 0xfe80, 0x10, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe81) == 0);

  BbcMachineWrite(bbc, 0xfe80, 0x3a);
  BbcMachineWrite(bbc, 0xfe81, 0x23);
  BbcMachineWrite(bbc, 0xfe81, 0x20);
  BbcMachineWrite(bbc, 0xfe80, 0x53);
  BbcMachineWrite(bbc, 0xfe81, 0x00);
  BbcMachineWrite(bbc, 0xfe81, 0x00);
  BbcMachineWrite(bbc, 0xfe81, 0x21);
  EXPECT(WaitBits(bbc, 0xfe80, 0x04, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe84) == 0x42);
  for (i = 1; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe80, 0x04, 80));
    (void)BbcMachineRead(bbc, 0xfe84);
  }
  EXPECT(WaitBits(bbc, 0xfe80, 0x10, 80));
  (void)BbcMachineRead(bbc, 0xfe81);

  BbcMachineWrite(bbc, 0xfe80, 0x3a);
  BbcMachineWrite(bbc, 0xfe81, 0x23);
  BbcMachineWrite(bbc, 0xfe81, 0x00);
  BbcMachineWrite(bbc, 0xfe80, 0x4b);
  BbcMachineWrite(bbc, 0xfe81, 0);
  BbcMachineWrite(bbc, 0xfe81, 1);
  BbcMachineWrite(bbc, 0xfe81, 0x21);
  for (i = 0; i < 256; i++) {
    EXPECT(WaitBits(bbc, 0xfe80, 0x04, 80));
    BbcMachineWrite(bbc, 0xfe84, 0x5a);
  }
  EXPECT(WaitBits(bbc, 0xfe80, 0x10, 80));
  EXPECT(BbcMachineRead(bbc, 0xfe81) == 0);
  EXPECT(WaitTrial(NULL, &drive0, 1));
  EXPECT(drive0.written != NULL);
  EXPECT(drive0.written_len == sizeof(image));
  if (drive0.written != NULL && drive0.written_len == sizeof(image)) {
    EXPECT(drive0.written[0] == 0);
    EXPECT(drive0.written[255] == 255);
    EXPECT(drive0.written[256] == 0x5a);
    EXPECT(drive0.written[511] == 0x5a);
  }

  BbcMachineDestroy(bbc);
  pthread_join(thread0, NULL);
  pthread_join(thread2, NULL);
  pthread_join(thread_refused, NULL);
  pthread_mutex_destroy(&drive0.mu);
  pthread_mutex_destroy(&drive2.mu);
  pthread_mutex_destroy(&refused.mu);
  free(drive0.written);
}

struct CassetteTrial {
  int port;
  const uint8_t* image;
  size_t length;
  pthread_mutex_t mu;
  int ready;
  int failed;
  int rewind;
  int rewound;
  int stop;
  uint8_t* written;
  size_t written_len;
};

static int Contains(const uint8_t* data, size_t length, const uint8_t* needle, size_t n) {
  size_t i;
  if (n == 0 || data == NULL || length < n) {
    return 0;
  }
  for (i = 0; i + n <= length; i++) {
    if (memcmp(data + i, needle, n) == 0) {
      return 1;
    }
  }
  return 0;
}

static void* CassetteTrialMain(void* arg) {
  struct CassetteTrial* trial = (struct CassetteTrial*)arg;
  int fd;
  int code = CASSETTE_ERR;
  char reply[64];
  uint8_t* written = NULL;
  size_t written_len = 0;
  fd = CumanaConnect(trial->port);
  if (fd < 0 ||
      CassetteSendInsert(fd, 0, "blank.uef", trial->image, trial->length) != 0 ||
      CassetteReadReply(fd, &code, reply, sizeof(reply)) != 0 || code != CASSETTE_OK) {
    if (fd >= 0) {
      SocketClose(fd);
    }
    pthread_mutex_lock(&trial->mu);
    trial->failed = 1;
    trial->ready = 1;
    pthread_mutex_unlock(&trial->mu);
    return NULL;
  }
  pthread_mutex_lock(&trial->mu);
  trial->ready = 1;
  pthread_mutex_unlock(&trial->mu);
  if (CassetteReadImage(fd, &written, &written_len) == 0) {
    pthread_mutex_lock(&trial->mu);
    trial->written = written;
    trial->written_len = written_len;
    pthread_mutex_unlock(&trial->mu);
  }
  for (;;) {
    int rewind = 0;
    int stop = 0;
    pthread_mutex_lock(&trial->mu);
    rewind = trial->rewind && !trial->rewound;
    stop = trial->stop;
    pthread_mutex_unlock(&trial->mu);
    if (stop) {
      break;
    }
    if (rewind) {
      char mark = CASSETTE_REWIND;
      if (CassetteSendAll(fd, &mark, 1) != 0) {
        break;
      }
      pthread_mutex_lock(&trial->mu);
      trial->rewound = 1;
      pthread_mutex_unlock(&trial->mu);
    }
    usleep(1000);
  }
  SocketClose(fd);
  return NULL;
}

static int WaitCassette(BbcMachine* bbc, struct CassetteTrial* trial, int want_write, int want_rewind) {
  int i;
  for (i = 0; i < 2000; i++) {
    int done;
    pthread_mutex_lock(&trial->mu);
    done = trial->failed ||
           (trial->ready && (!want_write || trial->written != NULL) &&
            (!want_rewind || trial->rewound));
    pthread_mutex_unlock(&trial->mu);
    if (done) {
      return 1;
    }
    if (bbc != NULL) {
      BbcMachineAdvance(bbc, 200);
    }
    usleep(1000);
  }
  return 0;
}

static void TestCassette(void) {
  char path_buf[512];
  const char* path = TempPath(path_buf, sizeof(path_buf), "bbc-uef-test.uef");
  uint8_t uef[] = {
      'U', 'E', 'F', ' ', 'F', 'i', 'l', 'e', '!', 0, 0x0a, 0,
      0x10, 0x01, 2, 0, 0, 0, 0x80, 0x02,
      0x00, 0x01, 1, 0, 0, 0, 0x2a,
  };
  uint8_t blank[] = {'U', 'E', 'F', ' ', 'F', 'i', 'l', 'e', '!', 0, 0x0a, 0};
  uint8_t saved[] = {0x00, 0x01, 2, 0, 0, 0, 0x2a, 0x55};
  struct CassetteTrial trial;
  pthread_t thread;
  BbcMachine* bbc;
  int port;
  int i;
  int played = 0;
  WriteImage(path, uef, sizeof(uef));
  bbc = BbcMachineCreate();
  EXPECT(BbcMachineLoadTape(bbc, path));
  BbcMachineWrite(bbc, 0xfe08, 0x03);
  BbcMachineWrite(bbc, 0xfe08, 0x15);
  BbcMachineWrite(bbc, 0xfe10, 0xc0);
  BbcMachineAdvance(bbc, 1);
  for (i = 0; i < 32; i++) {
    EXPECT((BbcMachineRead(bbc, 0xfe08) & 0x05) == 0x05);
    EXPECT(BbcMachineRead(bbc, 0xfe09) == 0xaa);
    BbcMachineAdvance(bbc, 2000);
  }
  EXPECT((BbcMachineRead(bbc, 0xfe08) & 0x05) == 0x01);
  EXPECT(BbcMachineRead(bbc, 0xfe09) == 0x2a);
  BbcMachineDestroy(bbc);
  remove(path);

  memset(&trial, 0, sizeof(trial));
  bbc = BbcMachineCreate();
  EXPECT(bbc != NULL);
  port = BbcMachineListenTapes(bbc, 0);
  EXPECT(port > 0);
  pthread_mutex_init(&trial.mu, NULL);
  trial.port = port;
  trial.image = blank;
  trial.length = sizeof(blank);
  EXPECT(pthread_create(&thread, NULL, CassetteTrialMain, &trial) == 0);
  EXPECT(WaitCassette(bbc, &trial, 0, 0));
  EXPECT(!trial.failed);
  BbcMachineWrite(bbc, 0xfe08, 0x03);
  BbcMachineWrite(bbc, 0xfe08, 0x15);
  BbcMachineWrite(bbc, 0xfe10, 0xc0);
  BbcMachineWrite(bbc, 0xfe09, 0x2a);
  BbcMachineAdvance(bbc, 2000);
  BbcMachineWrite(bbc, 0xfe09, 0x55);
  BbcMachineAdvance(bbc, 2000);
  BbcMachineWrite(bbc, 0xfe10, 0x40);
  EXPECT(WaitCassette(bbc, &trial, 1, 0));
  EXPECT(trial.written != NULL);
  EXPECT(Contains(trial.written, trial.written_len, saved, sizeof(saved)));
  pthread_mutex_lock(&trial.mu);
  trial.rewind = 1;
  pthread_mutex_unlock(&trial.mu);
  EXPECT(WaitCassette(bbc, &trial, 1, 1));
  BbcMachineWrite(bbc, 0xfe10, 0xc0);
  for (i = 0; i < 8 && !played; i++) {
    BbcMachineAdvance(bbc, 40000);
    if ((BbcMachineRead(bbc, 0xfe08) & 0x01) != 0 && BbcMachineRead(bbc, 0xfe09) == 0x2a) {
      played = 1;
    }
  }
  EXPECT(played);
  pthread_mutex_lock(&trial.mu);
  trial.stop = 1;
  pthread_mutex_unlock(&trial.mu);
  pthread_join(thread, NULL);
  BbcMachineDestroy(bbc);
  pthread_mutex_destroy(&trial.mu);
  free(trial.written);

  {
    char saved_buf[512];
    const char* saved_path = TempPath(saved_buf, sizeof(saved_buf), "bbc-tape-saved.uef");
    uint8_t marker[] = {0x2a};
    uint8_t* back = NULL;
    size_t back_len = 0;
    FILE* fp;
    WriteImage(saved_path, blank, sizeof(blank));
    bbc = BbcMachineCreate();
    EXPECT(BbcMachineLoadTape(bbc, saved_path));
    BbcMachineWrite(bbc, 0xfe08, 0x03);
    BbcMachineWrite(bbc, 0xfe08, 0x15);
    BbcMachineWrite(bbc, 0xfe10, 0xc0);
    BbcMachineWrite(bbc, 0xfe09, 0x2a);
    BbcMachineAdvance(bbc, 2000);
    BbcMachineWrite(bbc, 0xfe10, 0x40);
    fp = fopen(saved_path, "rb");
    EXPECT(fp != NULL);
    if (fp != NULL) {
      EXPECT(fseek(fp, 0, SEEK_END) == 0);
      back_len = (size_t)ftell(fp);
      EXPECT(fseek(fp, 0, SEEK_SET) == 0);
      back = (uint8_t*)malloc(back_len > 0 ? back_len : 1);
      EXPECT(back != NULL && fread(back, 1, back_len, fp) == back_len);
      fclose(fp);
    }
    EXPECT(Contains(back, back_len, marker, sizeof(marker)));
    free(back);
    BbcMachineDestroy(bbc);
    remove(saved_path);
  }
}

static void EconetSend(BbcMachine* bbc, const uint8_t* data, int length) {
  int sent = 0;
  int spins = 0;
  while (sent < length && spins < 1000) {
    if ((BbcMachineRead(bbc, 0xfea0) & 0x40) != 0) {
      BbcMachineWrite(bbc, sent + 1 == length ? 0xfea3 : 0xfea2, data[sent]);
      sent++;
    } else {
      BbcMachineAdvance(bbc, 80);
      spins++;
    }
  }
  EXPECT(sent == length);
}

static void TestEconet(void) {
  BbcMachine* a;
  BbcMachine* b;
  BbcMachine* master;
  uint8_t frame[5] = {0x02, 0x00, 0x01, 0x00, 0x81};
  uint8_t got[5];
  uint8_t ack[4] = {0x01, 0x00, 0x02, 0x00};
  int n = 0;
  int i;
  bool saw_nmi = false;
  a = BbcMachineCreate();
  b = BbcMachineCreate();
  EXPECT(a != NULL && b != NULL);
  EXPECT(BbcMachineRead(a, 0xfea0) == 0xfe);
  EXPECT(BbcMachineOpenEconet(a, 0, 28179) == -1);
  EXPECT(BbcMachineRead(a, 0xfe18) == 0xfe);
  EXPECT(BbcMachineOpenEconet(a, 1, 28179) != 0);
  EXPECT(BbcMachineOpenEconet(b, 2, 28179) != 0);
  EXPECT(BbcMachineRead(a, 0xfe18) == 1);
  EXPECT(BbcMachineRead(a, 0xfe1f) == 1);
  EXPECT(BbcMachineRead(b, 0xfe18) == 2);
  BbcMachineWrite(a, 0xfe18, 0x5a);
  EXPECT(BbcMachineRead(a, 0xfe18) == 1);
  EXPECT(BbcMachineRead(a, 0xfea4) == BbcMachineRead(a, 0xfea0));
  EXPECT((BbcMachineRead(a, 0xfea0) & 0x40) == 0);
  BbcMachineWrite(a, 0xfea0, 0x00);
  BbcMachineWrite(b, 0xfea0, 0x00);
  EXPECT((BbcMachineRead(a, 0xfea0) & 0x40) != 0);

  BbcMachineWrite(a, 0xfea2, 0x11);
  BbcMachineAdvance(a, 80);
  BbcMachineAdvance(a, 80);
  EXPECT((BbcMachineRead(a, 0xfea0) & 0x20) != 0);
  BbcMachineWrite(a, 0xfea1, 0x40);
  EXPECT((BbcMachineRead(a, 0xfea0) & 0x20) == 0);

  BbcMachineWrite(b, 0xfea0, 0x01);
  BbcMachineWrite(b, 0xfea1, 0x10);
  BbcMachineWrite(b, 0xfea0, 0x00);
  BbcMachineAdvance(b, 200);
  EXPECT((BbcMachineRead(b, 0xfea0) & 0x08) != 0);
  BbcMachineWrite(b, 0xfea1, 0x20);
  EXPECT((BbcMachineRead(b, 0xfea0) & 0x08) == 0);
  BbcMachineWrite(b, 0xfea0, 0x01);
  BbcMachineWrite(b, 0xfea1, 0x00);
  BbcMachineWrite(b, 0xfea0, 0x02);
  BbcMachineWrite(b, 0xfe20, 0x00);
  EconetSend(a, frame, 5);
  for (i = 0; i < 40 && n < 5; i++) {
    BbcMachineAdvance(a, 80);
    BbcMachineAdvance(b, 80);
    if (BbcMachineNmiPending(b)) {
      saw_nmi = true;
    }
    while (n < 5 && (BbcMachineRead(b, 0xfea0) & 0x01) != 0) {
      got[n++] = BbcMachineRead(b, 0xfea2);
    }
  }
  EXPECT(saw_nmi);
  EXPECT(n == 5);
  EXPECT(got[0] == 0x02 && got[1] == 0x00 && got[2] == 0x01 && got[3] == 0x00 && got[4] == 0x81);
  EXPECT((BbcMachineRead(b, 0xfea1) & 0x02) != 0);
  BbcMachineClearNmi(b);
  BbcMachineWrite(b, 0xfe18, 0x00);
  BbcMachineWrite(b, 0xfea1, 0x20);
  EconetSend(a, ack, 1);
  for (i = 0; i < 20; i++) {
    BbcMachineAdvance(a, 80);
    BbcMachineAdvance(b, 80);
  }
  EXPECT(!BbcMachineNmiPending(b));
  EXPECT((BbcMachineRead(b, 0xfea0) & 0x01) != 0);
  EXPECT(BbcMachineRead(b, 0xfea2) == 0x01);
  EXPECT((BbcMachineRead(b, 0xfea1) & 0x10) != 0);

  master = BbcMachineCreate();
  EXPECT(BbcMachineSetModel(master, BBC_MACHINE_MASTER));
  EXPECT(BbcMachineOpenEconet(master, 240, 28180) != 0);
  EXPECT(BbcMachineRead(master, 0xfe18) == 240);
  BbcMachineWrite(master, 0xfe3c, 0x00);
  BbcMachineWrite(master, 0xfea0, 0x06);
  EXPECT(BbcMachineNmiPending(master));
  BbcMachineClearNmi(master);
  BbcMachineWrite(master, 0xfe38, 0x00);
  BbcMachineAdvance(master, 80);
  EXPECT(!BbcMachineNmiPending(master));

  BbcMachineDestroy(master);
  BbcMachineDestroy(a);
  BbcMachineDestroy(b);
}

// A second emulator process is only a socket on the same port. It must
// hear a frame this process sends, so multicast loopback stays on.
static void TestEconetOtherProcess(void) {
  uint8_t frame[5] = {0x04, 0x00, 0x03, 0x00, 0x99};
  uint8_t buf[64];
  int on = 1;
  int fd;
  int i;
  ssize_t n = -1;
  struct sockaddr_in addr;
  struct ip_mreq mreq;
  BbcMachine* c;
  EXPECT(SocketStartup());
  fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
  EXPECT(fd >= 0);
  if (fd < 0) {
    return;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof(on));
#endif
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(28181);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  EXPECT(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
  memset(&mreq, 0, sizeof(mreq));
  inet_pton(AF_INET, "239.255.19.82", &mreq.imr_multiaddr);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  EXPECT(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) == 0);
  SocketSetNonBlocking(fd);

  c = BbcMachineCreate();
  EXPECT(c != NULL);
  EXPECT(BbcMachineOpenEconet(c, 3, 28181) != 0);
  BbcMachineWrite(c, 0xfea0, 0x00);
  EconetSend(c, frame, 5);
  for (i = 0; i < 40 && n < 0; i++) {
    struct pollfd p;
    BbcMachineAdvance(c, 80);
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    if (SocketPoll(&p, 1, 25) == 1) {
      n = recvfrom(fd, (char*)buf, (int)sizeof(buf), 0, NULL, NULL);
    }
  }
  EXPECT(n == 14 + 5);
  EXPECT(n >= 14 && memcmp(buf, "ECONET01", 8) == 0);
  EXPECT(n == 14 + 5 && memcmp(buf + 14, frame, 5) == 0);
  BbcMachineDestroy(c);
  SocketClose(fd);
}

int main(void) {
  uint8_t* ram = calloc(65536, 1);
  BbcMachine* bbc = BbcMachineCreate();
  EXPECT(ram != NULL && bbc != NULL);
  if (ram != NULL && bbc != NULL) {
    BbcMachineSetRam(bbc, ram);
    TestFredJim(bbc);
    TestSheila(bbc, ram);
    TestOsAndPpm(bbc, ram);
    TestKeyboardAndSound(bbc);
    TestUlaSplit();
    TestAddressRupture();
    TestTeletextBeam();
    TestDisc();
    TestCumana();
    TestCassette();
    TestPeripherals();
    TestTeletextControls();
    TestRomDirectory();
    TestMaster();
    TestEconet();
    TestEconetOtherProcess();
  }
  BbcMachineDestroy(bbc);
  free(ram);
  if (g_failures != 0) {
    fprintf(stderr, "%d BBC hardware checks failed\n", g_failures);
    return 1;
  }
  return 0;
}
