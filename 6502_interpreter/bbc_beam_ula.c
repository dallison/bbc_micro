#include "bbc_beam_ula.h"
#include "bbc_beam_ula_hw.h"

#include <limits.h>
#include <stddef.h>

static const uint8_t kRevsPal0[16] = {0x03, 0x13, 0x23, 0x33, 0x43, 0x53, 0x63, 0x73,
                                      0x84, 0x94, 0xa4, 0xb4, 0xc4, 0xd4, 0xe4, 0xf4};
static const uint8_t kRevsPal2[16] = {0x07, 0x17, 0x47, 0x57, 0x23, 0x33, 0x63, 0x73,
                                      0x80, 0x90, 0xc0, 0xd0, 0xa5, 0xb5, 0xe5, 0xf5};
static const uint8_t kRevsPal3Patch[4] = {0x26, 0x36, 0x66, 0x76};
static const uint8_t kRevsPal4Patch[4] = {0xa1, 0xb1, 0xe1, 0xf1};

static const BbcBeamUlaBand kRevsBands[] = {
    {18, 0x88, kRevsPal0, NULL, 0, false},
    {82, 0xc4, NULL, NULL, 0, true},
    {101, 0xc4, kRevsPal2, NULL, 0, false},
    {167, 0xc4, kRevsPal2, kRevsPal3Patch, 4, false},
    {INT_MAX, 0xc4, kRevsPal2, kRevsPal4Patch, 4, false},
};

static const BbcBeamUlaProfile kRevsProfile = {
    "revs_custom_mode5",
    kRevsBands,
    (int)(sizeof(kRevsBands) / sizeof(kRevsBands[0])),
    true,
    true,
};

static bool MatchesRevsCustomScreen(const struct BbcMachine* bbc) {
  uint16_t start;
  if (bbc == NULL || BbcHwTeletext(bbc)) {
    return false;
  }
  if ((BbcHwCrtc(bbc, 6) & 0x7f) != 26) {
    return false;
  }
  start = (uint16_t)(((BbcHwCrtc(bbc, 12) << 8) | BbcHwCrtc(bbc, 13)) & 0x3fff);
  return start == 0x0b50 || start == 0x0b00;
}

static const BbcBeamUlaProfile* LiveBeamUlaProfile(const struct BbcMachine* bbc) {
  if (MatchesRevsCustomScreen(bbc)) {
    return &kRevsProfile;
  }
  return NULL;
}

static void ApplyBand(struct BbcMachine* bbc, const BbcBeamUlaBand* band) {
  int i;
  if (band == NULL) {
    return;
  }
  BbcHwForceUlaControl(bbc, band->control);
  if (band->palette_all_blue) {
    for (i = 0; i < 16; i++) {
      BbcHwForceUlaPalette(bbc, (uint8_t)((i << 4) | 3));
    }
    return;
  }
  if (band->palette16 != NULL) {
    for (i = 0; i < 16; i++) {
      BbcHwForceUlaPalette(bbc, band->palette16[i]);
    }
  }
  if (band->palette_patch != NULL && band->palette_patch_count > 0) {
    for (i = 0; i < band->palette_patch_count; i++) {
      BbcHwForceUlaPalette(bbc, band->palette_patch[i]);
    }
  }
}

static int BandIndexForY(const BbcBeamUlaProfile* profile, int y) {
  int i;
  if (profile == NULL || profile->bands == NULL || profile->band_count <= 0) {
    return -1;
  }
  for (i = 0; i < profile->band_count; i++) {
    if (y < profile->bands[i].y_max) {
      return i;
    }
  }
  return profile->band_count - 1;
}

static void ApplyBandForY(struct BbcMachine* bbc, int y) {
  const BbcBeamUlaProfile* profile = LiveBeamUlaProfile(bbc);
  int band;
  if (profile == NULL) {
    return;
  }
  band = BandIndexForY(profile, y);
  if (band < 0 || band == BbcHwBeamUlaBand(bbc)) {
    return;
  }
  BbcHwBeamUlaSetBand(bbc, band);
  ApplyBand(bbc, &profile->bands[band]);
}

void BbcMachineBeamUlaRefresh(struct BbcMachine* bbc) {
  if (bbc == NULL) {
    return;
  }
  // CRTC is often programmed one register at a time; do not cache a profile
  // pointer that can drop out mid-sequence. Just invalidate band selection.
  BbcHwBeamUlaSetBand(bbc, -1);
}

bool BbcMachineBeamUlaOverride(const struct BbcMachine* bbc) {
  return LiveBeamUlaProfile(bbc) != NULL;
}

const BbcBeamUlaProfile* BbcMachineBeamUlaProfile(const struct BbcMachine* bbc) {
  return LiveBeamUlaProfile(bbc);
}

bool BbcMachineSplitScreenTimers(const struct BbcMachine* bbc) {
  if (bbc == NULL || BbcHwTeletext(bbc)) {
    return false;
  }
  if (BbcHwUserViaT1Chained(bbc)) {
    return true;
  }
  return BbcHwSysViaT1Chained(bbc);
}

bool BbcMachineNeedsRealtimeVideo(const struct BbcMachine* bbc) {
  return BbcMachineBeamUlaOverride(bbc);
}

void BbcMachineBeamUlaFlyback(struct BbcMachine* bbc) {
  if (bbc == NULL) {
    return;
  }
  BbcHwBeamUlaSetBand(bbc, -1);
  BbcHwBeamUlaSetLockGuest(bbc, false);
}

void BbcMachineBeamUlaFieldBegin(struct BbcMachine* bbc) {
  const BbcBeamUlaProfile* profile;
  if (bbc == NULL) {
    return;
  }
  profile = LiveBeamUlaProfile(bbc);
  if (profile != NULL && profile->lock_guest_ula) {
    BbcHwBeamUlaSetLockGuest(bbc, true);
  }
}

void BbcMachineBeamUlaPreparePixelRow(struct BbcMachine* bbc, int beam_y) {
  if (bbc == NULL || !BbcMachineBeamUlaOverride(bbc)) {
    return;
  }
  BbcHwBeamUlaSetBand(bbc, -1);
  ApplyBandForY(bbc, beam_y);
}

void BbcMachineBeamUlaApplyAtScanline(struct BbcMachine* bbc, int y) {
  if (bbc == NULL || !BbcMachineBeamUlaOverride(bbc)) {
    return;
  }
  ApplyBandForY(bbc, y);
}

bool BbcMachineBeamUlaStableCrtcVsync(const struct BbcMachine* bbc) {
  const BbcBeamUlaProfile* profile = BbcMachineBeamUlaProfile(bbc);
  return profile != NULL && profile->stable_crtc_vsync;
}

bool BbcMachineBeamUlaGuestLocked(const struct BbcMachine* bbc) {
  return bbc != NULL && BbcHwBeamUlaLockGuest(bbc);
}
