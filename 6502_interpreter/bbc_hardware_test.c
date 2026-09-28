#include "bbc_hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;

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
  EXPECT(BbcFrameHeight(bbc) == 250);
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
  // bottom scanline of the cell at the cursor address.
  BbcPixel(bbc, 0, 9, rgb);
  EXPECT(RgbEq(rgb, 0, 0, 0));
  BbcMachineWrite(bbc, 0xfe00, 14);
  BbcMachineWrite(bbc, 0xfe01, 0x7c);
  BbcMachineWrite(bbc, 0xfe00, 15);
  BbcMachineWrite(bbc, 0xfe01, 0x00);
  BbcMachineWrite(bbc, 0xfe00, 10);
  BbcMachineWrite(bbc, 0xfe01, 0x12);
  BbcMachineRender(bbc);
  BbcPixel(bbc, 0, 9, rgb);
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
  const char* os_path = "/tmp/bbc-test-os.rom";
  const char* ppm_path = "/tmp/bbc-test-screen.ppm";
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
  EXPECT(BbcFrameHeight(bbc) > 200);
  EXPECT(BbcFrameHeight(bbc) < 320);
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
  const char* ssd = "/tmp/bbc-disc-test.ssd";
  const char* locked = "/tmp/bbc-disc-locked.ssd";
  const char* adf = "/tmp/bbc-disc-test.adf";
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
  const char* tape_path = "/tmp/bbc-tape-test.bin";
  const char* disc_path = "/tmp/bbc-format-test.ssd";
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
  BbcPixel(bbc, 16, 4, rgb);
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
  char dir[] = "/tmp/bbc-roms-XXXXXX";
  char path[512];
  BbcRomFile files[BBC_ROM_IMAGE_MAX];
  int count;
  int index;
  uint8_t* ram;
  BbcMachine* bbc;
  bool skip[16];
  EXPECT(mkdtemp(dir) != NULL);
  WriteNamed(dir, "os-1.20.rom", 0x11);
  WriteNamed(dir, "15-basic2.rom", 0x22);
  WriteNamed(dir, "14-adfs-1.30.rom", 0x33);
  WriteNamed(dir, "4.rom", 0x44);
  WriteNamed(dir, "notes.txt", 0x55);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX);
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

  WriteNamed(dir, "junk.rom", 0x66);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX);
  EXPECT(count < 0);
  snprintf(path, sizeof(path), "%s/junk.rom", dir);
  remove(path);
  WriteNamed(dir, "15-other.rom", 0x77);
  count = BbcMachineListRomDirectory(dir, files, BBC_ROM_IMAGE_MAX);
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
  const char* os_path = "/tmp/bbc-master-os.rom";
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
    TestPeripherals();
    TestTeletextControls();
    TestRomDirectory();
    TestMaster();
  }
  BbcMachineDestroy(bbc);
  free(ram);
  if (g_failures != 0) {
    fprintf(stderr, "%d BBC hardware checks failed\n", g_failures);
    return 1;
  }
  return 0;
}
