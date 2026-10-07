//
//  tube.h
//  Ferranti Tube ULA between the BBC and a second processor.
//
//  The BBC sees the eight registers at &FEE0. The parasite sees the same
//  FIFOs, with the directions reversed, at &FEF8. An unfitted socket is not
//  this device: the BBC still reads &FE there.
//

#ifndef tube_h
#define tube_h

#include <stdbool.h>
#include <stdint.h>

typedef struct Tube Tube;

Tube* TubeCreate(void);
void TubeDestroy(Tube* tube);
// Power-on state. Register 3 holds one unused parasite-to-host byte so a
// parasite NMI is not raised the moment the host enables it.
void TubeHardReset(Tube* tube);

uint8_t TubeHostRead(Tube* tube, int reg);
void TubeHostWrite(Tube* tube, int reg, uint8_t value);
uint8_t TubeParasiteRead(Tube* tube, int reg);
void TubeParasiteWrite(Tube* tube, int reg, uint8_t value);

bool TubeHostIrq(const Tube* tube);
bool TubeParasiteIrq(const Tube* tube);
// Edge-triggered, like the 6502 NMI pin. Ack when the parasite takes it.
bool TubeParasiteNmi(const Tube* tube);
void TubeParasiteNmiAck(Tube* tube);
// Bit P of the control register. The parasite clock is held.
bool TubeParasiteHeld(const Tube* tube);

#endif
