#ifndef bbc_beam_ula_h
#define bbc_beam_ula_h

#include <stdbool.h>
#include <stdint.h>

struct BbcMachine;

typedef struct {
  int y_max;
  uint8_t control;
  const uint8_t* palette16;
  const uint8_t* palette_patch;
  int palette_patch_count;
  bool palette_all_blue;
} BbcBeamUlaBand;

typedef struct {
  const char* id;
  const BbcBeamUlaBand* bands;
  int band_count;
  bool lock_guest_ula;
  bool stable_crtc_vsync;
} BbcBeamUlaProfile;

// Re-evaluate CRTC/VIA state and attach a built-in profile when matched.
void BbcMachineBeamUlaRefresh(struct BbcMachine* bbc);

bool BbcMachineBeamUlaOverride(const struct BbcMachine* bbc);
const BbcBeamUlaProfile* BbcMachineBeamUlaProfile(const struct BbcMachine* bbc);

// Chained 6522 timer 1 on either VIA (Elite, Revs, others).
bool BbcMachineSplitScreenTimers(const struct BbcMachine* bbc);

// Do not turbo or snapshot over the beam while this is true.
bool BbcMachineNeedsRealtimeVideo(const struct BbcMachine* bbc);

void BbcMachineBeamUlaFlyback(struct BbcMachine* bbc);
void BbcMachineBeamUlaFieldBegin(struct BbcMachine* bbc);
void BbcMachineBeamUlaPreparePixelRow(struct BbcMachine* bbc, int beam_y);
void BbcMachineBeamUlaApplyAtScanline(struct BbcMachine* bbc, int y);

bool BbcMachineBeamUlaStableCrtcVsync(const struct BbcMachine* bbc);
bool BbcMachineBeamUlaGuestLocked(const struct BbcMachine* bbc);

#endif
