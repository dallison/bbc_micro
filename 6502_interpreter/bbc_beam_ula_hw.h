#ifndef bbc_beam_ula_hw_h
#define bbc_beam_ula_hw_h

#include "bbc_beam_ula.h"

// Glue between bbc_beam_ula.c and the machine implementation in bbc_hardware.c.
struct BbcMachine;

bool BbcHwTeletext(const struct BbcMachine* bbc);
uint8_t BbcHwCrtc(const struct BbcMachine* bbc, int reg);
bool BbcHwUserViaT1Chained(const struct BbcMachine* bbc);
bool BbcHwSysViaT1Chained(const struct BbcMachine* bbc);
void BbcHwForceUlaControl(struct BbcMachine* bbc, uint8_t value);
void BbcHwForceUlaPalette(struct BbcMachine* bbc, uint8_t value);

void BbcHwBeamUlaSetBand(struct BbcMachine* bbc, int band);
int BbcHwBeamUlaBand(const struct BbcMachine* bbc);
void BbcHwBeamUlaSetLockGuest(struct BbcMachine* bbc, bool lock);
bool BbcHwBeamUlaLockGuest(const struct BbcMachine* bbc);

#endif
