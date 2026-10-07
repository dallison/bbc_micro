//
//  tube.c
//  The four Tube FIFOs and the control flags in register 1.
//
//  Writing the host's register 1 status sets or clears flags. Bit 7 of that
//  write is S: 1 sets the flags in the low six bits, 0 clears them. Bit 6
//  is T and empties the FIFOs. The flags, low bit first, are Q, I, J, M, V
//  and P. Q raises the BBC's IRQ when the parasite has written register 4.
//  I and J raise the parasite's IRQ for register 1 and register 4. M raises
//  the parasite's NMI from register 3. V makes register 3 a two-byte FIFO.
//  P holds the parasite in reset.
//

#include "tube.h"

#include <stdlib.h>
#include <string.h>

#define TUBE_Q 0x01
#define TUBE_I 0x02
#define TUBE_J 0x04
#define TUBE_M 0x08
#define TUBE_V 0x10
#define TUBE_P 0x20

typedef struct {
  uint8_t data[24];
  int depth;
  int n;
  int r;
  uint8_t last;
  bool avail;
  bool space;
} TubeFifo;

struct Tube {
  TubeFifo r1_hp;
  TubeFifo r1_ph;
  TubeFifo r2_hp;
  TubeFifo r2_ph;
  TubeFifo r3_hp;
  TubeFifo r3_ph;
  TubeFifo r4_hp;
  TubeFifo r4_ph;
  uint8_t flags;
  bool pnmi_level;
  bool pnmi_edge;
};

static void FifoInit(TubeFifo* fifo, int depth) {
  memset(fifo, 0, sizeof(*fifo));
  fifo->depth = depth;
  fifo->space = true;
  fifo->last = 0xff;
}

static void FifoClear(TubeFifo* fifo) {
  fifo->n = 0;
  fifo->r = 0;
  fifo->avail = false;
  fifo->space = true;
}

static bool FifoFull(const TubeFifo* fifo, bool one_byte) {
  if (one_byte) {
    return fifo->n >= 1;
  }
  return fifo->n >= fifo->depth;
}

static void FifoNotePair(TubeFifo* fifo) {
  if (fifo->n >= 2) {
    fifo->avail = true;
    fifo->space = false;
  }
  if (fifo->n == 0) {
    fifo->avail = false;
    fifo->space = true;
  }
}

static void FifoWrite(TubeFifo* fifo, uint8_t value, bool one_byte, bool pair) {
  if (FifoFull(fifo, one_byte || (pair && fifo->n >= 2))) {
    return;
  }
  if (fifo->n >= fifo->depth) {
    return;
  }
  int at = (fifo->r + fifo->n) % fifo->depth;
  fifo->data[at] = value;
  fifo->last = value;
  fifo->n++;
  if (pair) {
    FifoNotePair(fifo);
  }
}

static uint8_t FifoRead(TubeFifo* fifo, bool pair) {
  if (fifo->n <= 0) {
    return fifo->last;
  }
  uint8_t value = fifo->data[fifo->r];
  fifo->last = value;
  fifo->r = (fifo->r + 1) % fifo->depth;
  fifo->n--;
  if (pair) {
    FifoNotePair(fifo);
  }
  return value;
}

static bool FifoAvailable(const TubeFifo* fifo, bool pair) {
  if (pair) {
    return fifo->avail;
  }
  return fifo->n > 0;
}

static bool FifoSpace(const TubeFifo* fifo, bool one_byte, bool pair) {
  if (pair) {
    return fifo->space;
  }
  if (one_byte) {
    return fifo->n == 0;
  }
  return fifo->n < fifo->depth;
}

static uint8_t StatusByte(bool available, bool space) {
  return (uint8_t)((available ? 0x80 : 0) | (space ? 0x40 : 0) | 0x3f);
}

static void UpdateNmi(Tube* tube) {
  bool level = false;
  if ((tube->flags & TUBE_M) != 0) {
    if ((tube->flags & TUBE_V) != 0) {
      level = tube->r3_hp.n >= 2 || tube->r3_ph.n == 0;
    } else {
      level = tube->r3_hp.n >= 1 || tube->r3_ph.n == 0;
    }
  }
  if (level && !tube->pnmi_level) {
    tube->pnmi_edge = true;
  }
  tube->pnmi_level = level;
}

static void EnterPairMode(TubeFifo* fifo) {
  fifo->avail = fifo->n >= 2;
  fifo->space = fifo->n < 2;
}

static void Purge(Tube* tube) {
  FifoClear(&tube->r1_hp);
  FifoClear(&tube->r1_ph);
  FifoClear(&tube->r2_hp);
  FifoClear(&tube->r2_ph);
  FifoClear(&tube->r3_hp);
  FifoClear(&tube->r3_ph);
  FifoClear(&tube->r4_hp);
  FifoClear(&tube->r4_ph);
  // One byte in the parasite-to-host register 3 FIFO. Without it, enabling
  // the parasite NMI would fire while that FIFO is empty.
  tube->r3_ph.data[0] = 0;
  tube->r3_ph.n = 1;
  tube->r3_ph.last = 0;
  if ((tube->flags & TUBE_V) != 0) {
    EnterPairMode(&tube->r3_hp);
    EnterPairMode(&tube->r3_ph);
  }
  UpdateNmi(tube);
}

Tube* TubeCreate(void) {
  Tube* tube = calloc(1, sizeof(*tube));
  if (tube == NULL) {
    return NULL;
  }
  FifoInit(&tube->r1_hp, 1);
  FifoInit(&tube->r1_ph, 24);
  FifoInit(&tube->r2_hp, 1);
  FifoInit(&tube->r2_ph, 1);
  FifoInit(&tube->r3_hp, 2);
  FifoInit(&tube->r3_ph, 2);
  FifoInit(&tube->r4_hp, 1);
  FifoInit(&tube->r4_ph, 1);
  TubeHardReset(tube);
  return tube;
}

void TubeDestroy(Tube* tube) { free(tube); }

void TubeHardReset(Tube* tube) {
  if (tube == NULL) {
    return;
  }
  tube->flags = 0;
  tube->pnmi_level = false;
  tube->pnmi_edge = false;
  Purge(tube);
}

static bool Pair(const Tube* tube) { return (tube->flags & TUBE_V) != 0; }

uint8_t TubeHostRead(Tube* tube, int reg) {
  bool pair = Pair(tube);
  switch (reg & 7) {
    case 0:
      return (uint8_t)((FifoAvailable(&tube->r1_ph, false) ? 0x80 : 0) |
                        (FifoSpace(&tube->r1_hp, true, false) ? 0x40 : 0) |
                        (tube->flags & 0x3f));
    case 1:
      return FifoRead(&tube->r1_ph, false);
    case 2:
      return StatusByte(FifoAvailable(&tube->r2_ph, false),
                         FifoSpace(&tube->r2_hp, true, false));
    case 3:
      return FifoRead(&tube->r2_ph, false);
    case 4:
      return StatusByte(FifoAvailable(&tube->r3_ph, pair),
                         FifoSpace(&tube->r3_hp, !pair, pair));
    case 5: {
      uint8_t value = FifoRead(&tube->r3_ph, pair);
      UpdateNmi(tube);
      return value;
    }
    case 6:
      return StatusByte(FifoAvailable(&tube->r4_ph, false),
                         FifoSpace(&tube->r4_hp, true, false));
    default:
      return FifoRead(&tube->r4_ph, false);
  }
}

void TubeHostWrite(Tube* tube, int reg, uint8_t value) {
  bool pair = Pair(tube);
  switch (reg & 7) {
    case 0: {
      if ((value & 0x40) != 0) {
        Purge(tube);
      }
      if ((value & 0x80) != 0) {
        tube->flags = (uint8_t)(tube->flags | (value & 0x3f));
      } else {
        tube->flags = (uint8_t)(tube->flags & (uint8_t)~(value & 0x3f));
      }
      if ((tube->flags & TUBE_V) != 0) {
        EnterPairMode(&tube->r3_hp);
        EnterPairMode(&tube->r3_ph);
      }
      UpdateNmi(tube);
      break;
    }
    case 1:
      FifoWrite(&tube->r1_hp, value, true, false);
      break;
    case 3:
      FifoWrite(&tube->r2_hp, value, true, false);
      break;
    case 5:
      FifoWrite(&tube->r3_hp, value, !pair, pair);
      UpdateNmi(tube);
      break;
    case 7:
      FifoWrite(&tube->r4_hp, value, true, false);
      break;
    default:
      break;
  }
}

uint8_t TubeParasiteRead(Tube* tube, int reg) {
  bool pair = Pair(tube);
  switch (reg & 7) {
    case 0:
      return (uint8_t)((FifoAvailable(&tube->r1_hp, false) ? 0x80 : 0) |
                        (FifoSpace(&tube->r1_ph, false, false) ? 0x40 : 0) |
                        (tube->flags & 0x3f));
    case 1:
      return FifoRead(&tube->r1_hp, false);
    case 2:
      return StatusByte(FifoAvailable(&tube->r2_hp, false),
                         FifoSpace(&tube->r2_ph, true, false));
    case 3:
      return FifoRead(&tube->r2_hp, false);
    case 4:
      return StatusByte(FifoAvailable(&tube->r3_hp, pair),
                         FifoSpace(&tube->r3_ph, !pair, pair));
    case 5: {
      uint8_t value = FifoRead(&tube->r3_hp, pair);
      UpdateNmi(tube);
      return value;
    }
    case 6:
      return StatusByte(FifoAvailable(&tube->r4_hp, false),
                         FifoSpace(&tube->r4_ph, true, false));
    default:
      return FifoRead(&tube->r4_hp, false);
  }
}

void TubeParasiteWrite(Tube* tube, int reg, uint8_t value) {
  bool pair = Pair(tube);
  switch (reg & 7) {
    case 1:
      FifoWrite(&tube->r1_ph, value, false, false);
      break;
    case 3:
      FifoWrite(&tube->r2_ph, value, true, false);
      break;
    case 5:
      FifoWrite(&tube->r3_ph, value, !pair, pair);
      UpdateNmi(tube);
      break;
    case 7:
      FifoWrite(&tube->r4_ph, value, true, false);
      break;
    default:
      break;
  }
}

bool TubeHostIrq(const Tube* tube) {
  return tube != NULL && (tube->flags & TUBE_Q) != 0 && tube->r4_ph.n > 0;
}

bool TubeParasiteIrq(const Tube* tube) {
  if (tube == NULL) {
    return false;
  }
  if ((tube->flags & TUBE_I) != 0 && tube->r1_hp.n > 0) {
    return true;
  }
  return (tube->flags & TUBE_J) != 0 && tube->r4_hp.n > 0;
}

bool TubeParasiteNmi(const Tube* tube) {
  return tube != NULL && tube->pnmi_edge;
}

void TubeParasiteNmiAck(Tube* tube) {
  if (tube != NULL) {
    tube->pnmi_edge = false;
  }
}

bool TubeParasiteHeld(const Tube* tube) {
  return tube != NULL && (tube->flags & TUBE_P) != 0;
}
