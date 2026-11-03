//
//  bbc_mac.m
//  BBC Micro emulator: the 6502 interpreter and BBC hardware, with an
//  AppKit window for the video ULA and Core Audio for the SN76489.
//

#import <AudioToolbox/AudioToolbox.h>
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#import <IOKit/hid/IOHIDProperties.h>
#import <IOKit/hidsystem/IOHIDEventSystemClient.h>
#import <IOKit/hidsystem/IOHIDLib.h>
#import <IOKit/hidsystem/IOHIDParameter.h>
#import <IOKit/hidsystem/IOLLEvent.h>
#import <IOKit/hidsystem/ev_keymap.h>
#pragma clang diagnostic ignored "-Wnullability-completeness"

#include "6502_interpreter.h"
#include "bbc_hardware.h"
#include "bbc_session.h"

#include <mach/mach_time.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static const int kFrameCycles = 40000;

static bool KeyPosition(unsigned short key_code, int* column, int* row) {
  struct { unsigned short code; int column; int row; } keys[] = {
      {0, 1, 4},   {1, 1, 5},   {2, 2, 3},   {3, 3, 4},   {4, 4, 5},
      {5, 3, 5},   {6, 1, 6},   {7, 2, 4},   {8, 2, 5},   {9, 3, 6},
      {11, 4, 6},  {12, 0, 1},  {13, 1, 2},  {14, 2, 2},  {15, 3, 3},
      {16, 4, 4},  {17, 3, 2},  {18, 0, 3},  {19, 1, 3},  {20, 1, 1},
      {21, 2, 1},  {22, 4, 3},  {23, 3, 1},  {25, 6, 2},  {26, 4, 2},
      {27, 7, 1},  {28, 5, 1},  {29, 7, 2},  {30, 8, 5},  {31, 6, 3},
      {32, 5, 3},  {33, 8, 3},  {34, 5, 2},  {35, 7, 3},  {36, 9, 4},
      {37, 6, 5},  {38, 5, 4},  {39, 7, 4},  {40, 6, 4},  {41, 7, 5},
      {42, 8, 7},  {43, 6, 6},  {44, 8, 6},  {45, 5, 5},  {46, 5, 6},
      {47, 7, 6},  {48, 0, 6},  {49, 2, 6},  {50, 8, 1},  {51, 9, 5},
      {53, 0, 7},  {96, 4, 7},  {97, 5, 7},  {98, 6, 1},  {99, 3, 7},
      {100, 6, 7}, {101, 7, 7}, {109, 0, 2}, {118, 4, 1}, {119, 9, 6},
      {120, 2, 7}, {122, 1, 7}, {123, 9, 1}, {124, 9, 7}, {125, 9, 2},
      {126, 9, 3},
  };
  size_t i;
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (keys[i].code == key_code) {
      *column = keys[i].column;
      *row = keys[i].row;
      return true;
    }
  }
  return false;
}

// Master keypad, columns 10-12. The scan codes are the hardware rows the MOS
// looks up: 4 5 2 on row 7, then 6 7, 8 9, + - Return, / Delete ., # * and 0 1 3.
static bool MasterKeyPosition(unsigned short key_code, int* column, int* row) {
  struct { unsigned short code; int column; int row; } keys[] = {
      {82, 10, 6}, {83, 11, 6}, {84, 12, 7}, {85, 12, 6}, {86, 10, 7},
      {87, 11, 7}, {88, 10, 1}, {89, 11, 1}, {91, 10, 2}, {92, 11, 2},
      {65, 12, 4}, {67, 11, 5}, {69, 10, 3}, {75, 10, 4}, {78, 11, 3},
      {76, 12, 3}, {81, 10, 5}, {71, 11, 4},
  };
  size_t i;
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (keys[i].code == key_code) {
      *column = keys[i].column;
      *row = keys[i].row;
      return true;
    }
  }
  return false;
}

// Host keys currently held, and the BBC matrix cell each one is driving.
// shift_force is -1 to follow the host shift key, or 0/1 when the US
// character needs the opposite shift state from the one being held.
typedef struct {
  bool used;
  unsigned short code;
  int column;
  int row;
  int shift_force;
} HeldKey;

static bool g_host_shift = false;
static HeldKey g_held[16];

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

static void ReleaseHostKey(unsigned short key_code) {
  int i;
  for (i = 0; i < 16; i++) {
    if (!g_held[i].used || g_held[i].code != key_code) {
      continue;
    }
    BbcMachineSetKey(g_cpu.bbc, g_held[i].column, g_held[i].row, false);
    g_held[i].used = false;
  }
  SyncShift();
}

// The Mac Caps Lock light toggles, and that flag changes only on the press.
// A second flagsChanged arrives for the release with the flag left as it is.
// The MOS toggles its lock on the scan after it first sees the key, and only
// while that key is still down. A typing press therefore stays down for two
// frames and then comes up, so each press toggles once. Holding it for the
// whole time the light is on toggles on the way on and misses the way off.
// -game-caps leaves the key down for the whole hold, which Zalaga reads.
static bool g_caps_flag = false;
static bool g_caps_flag_ready = false;
static int g_caps_pulse = 0;

static void RememberCapsFlag(void) {
  if (g_caps_flag_ready) {
    return;
  }
  g_caps_flag = ([NSEvent modifierFlags] & NSEventModifierFlagCapsLock) != 0;
  g_caps_flag_ready = true;
}

static void ApplyMomentaryModifiers(NSEventModifierFlags flags) {
  if (!g_ready || g_cpu.bbc == NULL) {
    return;
  }
  g_host_shift = (flags & NSEventModifierFlagShift) != 0;
  BbcMachineSetKey(g_cpu.bbc, 1, 0, (flags & NSEventModifierFlagControl) != 0);
  SyncShift();
}

static void CapsLockEvent(NSEvent* event) {
  bool flag;
  bool pressed;
  if (!g_ready || g_cpu.bbc == NULL) {
    return;
  }
  RememberCapsFlag();
  flag = (event.modifierFlags & NSEventModifierFlagCapsLock) != 0;
  pressed = flag != g_caps_flag;
  if (!pressed) {
    if (BbcSessionGameCaps()) {
      BbcMachineSetKey(g_cpu.bbc, 0, 4, false);
    }
    return;
  }
  g_caps_flag = flag;
  BbcMachineSetKey(g_cpu.bbc, 0, 4, true);
  if (!BbcSessionGameCaps()) {
    g_caps_pulse = 2;
  }
}

static void EndCapsPulse(void) {
  if (g_caps_pulse <= 0 || g_cpu.bbc == NULL || BbcSessionGameCaps()) {
    return;
  }
  g_caps_pulse--;
  if (g_caps_pulse == 0) {
    BbcMachineSetKey(g_cpu.bbc, 0, 4, false);
  }
}

static void ReleaseStuckKeys(void) {
  int i;
  if (!g_ready || g_cpu.bbc == NULL) {
    return;
  }
  for (i = 0; i < 16; i++) {
    if (!g_held[i].used) {
      continue;
    }
    BbcMachineSetKey(g_cpu.bbc, g_held[i].column, g_held[i].row, false);
    g_held[i].used = false;
  }
  g_host_shift = false;
  g_caps_pulse = 0;
  BbcMachineSetKey(g_cpu.bbc, 0, 4, false);
  BbcMachineSetKey(g_cpu.bbc, 1, 0, false);
  SyncShift();
}

// Matrix key for the character that was typed. Modes 0-6 draw that MOS
// code: shift-3 is &23 (#), column 8 row 2 is &5F (_), and its shift is
// &60 (the pound sign). UK MODE 7 draws those three as the pound sign, #
// and a dash. That chip is the only place the shapes differ.
static bool GlyphToBbc(unichar ch, int* column, int* row, bool* need_shift) {
  static const int kDigit[10][2] = {
      {7, 2}, {0, 3}, {1, 3}, {1, 1}, {2, 1}, {3, 1}, {4, 3}, {4, 2}, {5, 1}, {6, 2},
  };
  struct {
    unichar ch;
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

// AppKit's function-key characters, F1 then F2 and so on. F10 is the BBC f0 key.
static bool FunctionCharacter(unichar ch, int* column, int* row) {
  static const int cells[10][2] = {
      {1, 7}, {2, 7}, {3, 7}, {4, 1}, {4, 7}, {5, 7}, {6, 1}, {6, 7}, {7, 7}, {0, 2},
  };
  if (ch < NSF1FunctionKey || ch > NSF1FunctionKey + 9) {
    return false;
  }
  *column = cells[ch - NSF1FunctionKey][0];
  *row = cells[ch - NSF1FunctionKey][1];
  return true;
}

static bool LetterKey(unsigned short key_code) {
  switch (key_code) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
    case 8: case 9: case 11: case 12: case 13: case 14: case 15: case 16:
    case 17: case 31: case 32: case 34: case 35: case 37: case 38: case 40:
    case 45: case 46:
      return true;
    default:
      return false;
  }
}

static void PressHostKey(NSEvent* event) {
  int column = 0;
  int row = 0;
  int shift_force = -1;
  bool mapped = false;
  bool control = (event.modifierFlags & NSEventModifierFlagControl) != 0;
  g_host_shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
  NSString* chars = event.characters;
  int slot;
  if (event.keyCode == 111) {
    W65C02InterpreterResetCpu(&g_cpu);
    return;
  }
  // The Kinesis Freestyle2 F12 key sends Command-Option-D until Fn is on.
  // keyCode 2 is the D key.
  if (event.keyCode == 2 && (event.modifierFlags & NSEventModifierFlagCommand) != 0 &&
      (event.modifierFlags & NSEventModifierFlagOption) != 0) {
    ApplyMomentaryModifiers(event.modifierFlags);
    W65C02InterpreterResetCpu(&g_cpu);
    return;
  }
  ReleaseHostKey(event.keyCode);
  if (!mapped && chars.length > 0 &&
      FunctionCharacter([chars characterAtIndex:0], &column, &row)) {
    mapped = true;
  }
  if (!mapped && g_cpu.bbc != NULL && BbcMachineIsMaster(g_cpu.bbc) &&
      MasterKeyPosition(event.keyCode, &column, &row)) {
    mapped = true;
  }
  if (!mapped && !control && !LetterKey(event.keyCode) && chars.length > 0) {
    unichar ch = [chars characterAtIndex:0];
    bool need_shift = false;
    if (ch >= 32 && GlyphToBbc(ch, &column, &row, &need_shift)) {
      mapped = true;
      if (ch != ' ' && need_shift != g_host_shift) {
        shift_force = need_shift ? 1 : 0;
      }
    }
  }
  if (!mapped && event.keyCode == 27) {
    // The key beside 0, used when AppKit did not hand over a character.
    // Shift types '_'. The BBC minus key's shift is '=', so the underscore
    // byte (&5F) is a different key and Shift has to be up.
    if (g_host_shift) {
      column = 8;
      row = 2;
      shift_force = 0;
    } else {
      column = 7;
      row = 1;
    }
    mapped = true;
  }
  if (!mapped) {
    mapped = KeyPosition(event.keyCode, &column, &row);
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
  g_held[slot].code = event.keyCode;
  g_held[slot].column = column;
  g_held[slot].row = row;
  g_held[slot].shift_force = shift_force;
  // Release Shift before the key is visible. A scan in between would turn
  // the underscore key into &60.
  SyncShift();
  BbcMachineSetKey(g_cpu.bbc, column, row, true);
}

@interface BbcView : NSView
@property(nonatomic) uint8_t* pixels;
@property(nonatomic) int pixWidth;
@property(nonatomic) int pixHeight;
@end

@implementation BbcView

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)mouseDown:(NSEvent*)event {
  (void)event;
  [[self window] makeFirstResponder:self];
}

- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;
  [[NSColor blackColor] setFill];
  NSRectFill(self.bounds);
  if (self.pixels == NULL || self.pixWidth <= 0 || self.pixHeight <= 0) {
    return;
  }
  unsigned char* planes = self.pixels;
  NSBitmapImageRep* rep =
      [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:&planes
                                               pixelsWide:self.pixWidth
                                               pixelsHigh:self.pixHeight
                                            bitsPerSample:8
                                          samplesPerPixel:3
                                                 hasAlpha:NO
                                                 isPlanar:NO
                                           colorSpaceName:NSDeviceRGBColorSpace
                                              bytesPerRow:self.pixWidth * 3
                                             bitsPerPixel:24];
  NSGraphicsContext* context = [NSGraphicsContext currentContext];
  NSImageInterpolation previous = context.imageInterpolation;
  // Integer scaling. Blending neighbouring scanlines turns the doubled
  // lines into grey bars.
  context.imageInterpolation = NSImageInterpolationNone;
  [rep drawInRect:self.bounds];
  context.imageInterpolation = previous;
}

- (void)keyDown:(NSEvent*)event {
  if (!g_ready || event.isARepeat) {
    return;
  }
  PressHostKey(event);
}

- (void)keyUp:(NSEvent*)event {
  if (!g_ready) {
    return;
  }
  ReleaseHostKey(event.keyCode);
}

- (void)flagsChanged:(NSEvent*)event {
  if (!g_ready) {
    return;
  }
  if (event.keyCode == 57) {
    CapsLockEvent(event);
  }
  ApplyMomentaryModifiers(event.modifierFlags);
}

@end

@interface BbcController : NSObject <NSApplicationDelegate>
@property(nonatomic) NSWindow* window;
@property(nonatomic) BbcView* view;
@property(nonatomic) NSTimer* timer;
@property(nonatomic) AudioQueueRef audioQueue;
@property(nonatomic) uint8_t* frameCopy;
@end

@implementation BbcController

static void AudioCallback(void* user, AudioQueueRef queue, AudioQueueBufferRef buffer) {
  BbcController* self = (__bridge BbcController*)user;
  int frames = (int)(buffer->mAudioDataBytesCapacity / sizeof(int16_t));
  int16_t* dst = buffer->mAudioData;
  int got = 0;
  if (g_ready && g_cpu.bbc != NULL) {
    got = BbcMachineReadAudio(g_cpu.bbc, dst, frames);
  }
  if (got < frames) {
    memset(dst + got, 0, (size_t)(frames - got) * sizeof(int16_t));
  }
  buffer->mAudioDataByteSize = (UInt32)(frames * sizeof(int16_t));
  AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
  (void)self;
}

- (void)startAudio {
  AudioStreamBasicDescription format;
  memset(&format, 0, sizeof(format));
  // A faster fixed clock produces samples faster. Play them at that rate so
  // the pitch and the envelopes stay locked to the CPU.
  {
    int mhz = g_cpu.clock_mhz > 0 ? g_cpu.clock_mhz : 2;
    format.mSampleRate = 48000.0 * (double)mhz / 2.0;
  }
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  format.mBitsPerChannel = 16;
  format.mChannelsPerFrame = 1;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = 2;
  format.mBytesPerPacket = 2;
  OSStatus status = AudioQueueNewOutput(&format, AudioCallback, (__bridge void*)self, NULL,
                                        NULL, 0, &_audioQueue);
  if (status != noErr && format.mSampleRate != 48000.0) {
    format.mSampleRate = 48000.0;
    status = AudioQueueNewOutput(&format, AudioCallback, (__bridge void*)self, NULL, NULL, 0,
                                 &_audioQueue);
  }
  if (status != noErr) {
    fprintf(stderr, "AudioQueueNewOutput failed (%d)\n", (int)status);
    return;
  }
  int i;
  for (i = 0; i < 3; i++) {
    AudioQueueBufferRef buffer = NULL;
    AudioQueueAllocateBuffer(self.audioQueue, 1024 * sizeof(int16_t), &buffer);
    buffer->mAudioDataByteSize = 1024 * sizeof(int16_t);
    memset(buffer->mAudioData, 0, buffer->mAudioDataByteSize);
    AudioQueueEnqueueBuffer(self.audioQueue, buffer, 0, NULL);
  }
  AudioQueueStart(self.audioQueue, NULL);
}

- (void)tick:(NSTimer*)timer {
  (void)timer;
  if (!g_ready || !g_cpu.running) {
    return;
  }
  if ([NSApp isActive]) {
    ApplyMomentaryModifiers([NSEvent modifierFlags]);
  }
  if (g_cpu.bbc != NULL && self.view != nil && self.window != nil &&
      !BbcMachineJoystickRemoteActive(g_cpu.bbc)) {
    NSPoint loc = [self.view convertPoint:[self.window mouseLocationOutsideOfEventStream]
                                  fromView:nil];
    NSRect bounds = self.view.bounds;
    int ax = 0x8000;
    int ay = 0x8000;
    if (bounds.size.width > 1.0 && bounds.size.height > 1.0 && NSPointInRect(loc, bounds)) {
      double nx = loc.x / bounds.size.width;
      double ny = loc.y / bounds.size.height;
      if (nx < 0.0) {
        nx = 0.0;
      }
      if (nx > 1.0) {
        nx = 1.0;
      }
      if (ny < 0.0) {
        ny = 0.0;
      }
      if (ny > 1.0) {
        ny = 1.0;
      }
      ax = (int)(nx * 65535.0);
      ay = (int)((1.0 - ny) * 65535.0);
    }
    BbcMachineSetAnalogue(g_cpu.bbc, 0, ax);
    BbcMachineSetAnalogue(g_cpu.bbc, 1, ay);
    NSUInteger buttons = [NSEvent pressedMouseButtons];
    BbcMachineSetFire(g_cpu.bbc, 0, (buttons & (NSUInteger)1) != 0);
    BbcMachineSetFire(g_cpu.bbc, 1, (buttons & (NSUInteger)2) != 0);
  }
  int cycles = 0;
  // One timer tick is 1/50 s. At 2 MHz that is one frame. A fixed faster
  // clock runs more frames in the same tick, with the hardware still
  // advancing one cycle per CPU cycle.
  int budget = kFrameCycles;
  if (g_cpu.clock_mhz > 0) {
    budget = kFrameCycles * g_cpu.clock_mhz / 2;
    if (budget < 1) {
      budget = 1;
    }
  }
  int limit = g_cpu.turbo ? 2000000 : budget;
  uint64_t started = mach_absolute_time();
  static mach_timebase_info_data_t timebase;
  static uint64_t slice = 0;
  if (slice == 0) {
    mach_timebase_info(&timebase);
    slice = 8000000ull * timebase.denom / timebase.numer;
  }
  while (cycles < limit && g_cpu.running) {
    int step = W65C02InterpreterStep(&g_cpu);
    if (step <= 0) {
      break;
    }
    cycles += step;
    if (!g_cpu.turbo && cycles >= budget) {
      break;
    }
    if (g_cpu.turbo && cycles > kFrameCycles && mach_absolute_time() - started > slice) {
      break;
    }
  }
  EndCapsPulse();
  if (g_cpu.bbc == NULL) {
    return;
  }
  {
    static int shown = -1;
    int caps = BbcMachineCapsLed(g_cpu.bbc) ? 1 : 0;
    int shift = BbcMachineShiftLed(g_cpu.bbc) ? 1 : 0;
    int motor = BbcMachineMotorOn(g_cpu.bbc) ? 1 : 0;
    int cmos = g_cpu.bbc_65c02 ? 1 : 0;
    int master = BbcMachineIsMaster(g_cpu.bbc) ? 1 : 0;
    int stamp = caps | (shift << 1) | (motor << 2) | (cmos << 3) | (master << 4);
    if (stamp != shown && self.window != nil) {
      NSString* title = master ? @"BBC Master" : @"BBC Micro";
      if (caps) {
        title = [title stringByAppendingString:@"  CAPS"];
      }
      if (shift) {
        title = [title stringByAppendingString:@"  SHIFT"];
      }
      if (motor) {
        title = [title stringByAppendingString:@"  MOTOR"];
      }
      if (master) {
        title = [title stringByAppendingString:@"  65SC12"];
      } else if (cmos) {
        title = [title stringByAppendingString:@"  65C02"];
      }
      self.window.title = title;
      shown = stamp;
    }
  }
  BbcMachinePresentFrame(g_cpu.bbc, g_cpu.turbo);
  int width = BbcFrameWidth(g_cpu.bbc);
  int height = BbcFrameHeight(g_cpu.bbc);
  if (width <= 0 || height <= 0) {
    return;
  }
  size_t bytes = (size_t)width * (size_t)height * 3;
  uint8_t* copy = realloc(self.frameCopy, bytes);
  if (copy == NULL) {
    return;
  }
  self.frameCopy = copy;
  const uint8_t* src = BbcFramebuffer(g_cpu.bbc);
  int y;
  for (y = 0; y < height; y++) {
    memcpy(copy + (size_t)y * (size_t)width * 3,
           src + (size_t)y * BBC_FB_WIDTH * 3, (size_t)width * 3);
  }
  self.view.pixels = copy;
  self.view.pixWidth = width;
  self.view.pixHeight = height;
  // The window size and 5:4 shape come from -scale at launch. Bitmap
  // pixels are twice as tall as they are wide, and that stretch is the
  // view filling the window. Refitting to the raw frame would drop the
  // stretch for a tall MODE 7 picture and change the size the user asked
  // for.
  [self.view setNeedsDisplay:YES];
}

// The live switch for an Apple keyboard's top row. 1 means the keys are
// F1-F12. 0 means brightness, volume, and the other pictures on the keys.
static int g_fkey_saved = -1;
static bool g_fkey_overridden = false;

static io_connect_t HidParamConnect(void) {
  io_service_t service;
  io_connect_t connect = MACH_PORT_NULL;
  service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass));
  if (service == IO_OBJECT_NULL) {
    return MACH_PORT_NULL;
  }
  if (IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &connect) != KERN_SUCCESS) {
    connect = MACH_PORT_NULL;
  }
  IOObjectRelease(service);
  return connect;
}

static int ReadFKeyMode(void) {
  io_connect_t connect = HidParamConnect();
  CFTypeRef value = NULL;
  int mode = -1;
  if (connect == MACH_PORT_NULL) {
    return -1;
  }
  if (IOHIDCopyCFTypeParameter(connect, CFSTR(kIOHIDFKeyModeKey), &value) == KERN_SUCCESS &&
      value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
    CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &mode);
  }
  if (value != NULL) {
    CFRelease(value);
  }
  IOServiceClose(connect);
  return mode;
}

static void WriteFKeyMode(int mode) {
  io_connect_t connect = HidParamConnect();
  CFNumberRef value;
  if (connect == MACH_PORT_NULL || mode < 0) {
    return;
  }
  value = CFNumberCreate(NULL, kCFNumberIntType, &mode);
  if (value != NULL) {
    IOHIDSetCFTypeParameter(connect, CFSTR(kIOHIDFKeyModeKey), value);
    CFRelease(value);
  }
  IOServiceClose(connect);
}

// Consumer-page volume up is the key in the F12 position when that row is
// the pictures (brightness, volume) rather than F1-F12. Turn it into a real
// F12 while this window is in front, then put the previous mapping back.
static const uint64_t kVolumeUpUsage = ((uint64_t)0x0c << 32) | 0x00e9;
static const uint64_t kF12Usage = ((uint64_t)0x07 << 32) | 0x0045;
static CFArrayRef g_key_map_saved = NULL;
static bool g_key_map_overridden = false;

static uint64_t MappingSource(CFDictionaryRef entry) {
  CFTypeRef value = CFDictionaryGetValue(entry, CFSTR(kIOHIDKeyboardModifierMappingSrcKey));
  uint64_t number = 0;
  if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
    CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &number);
  }
  return number;
}

static void UseVolumeUpAsBreak(bool enable) {
  IOHIDEventSystemClientRef client = IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
  CFTypeRef current;
  CFArrayRef existing;
  CFMutableArrayRef next;
  CFIndex i;
  CFIndex count;
  CFNumberRef source;
  CFNumberRef dest;
  CFDictionaryRef entry;
  const void* keys[2];
  const void* values[2];
  if (client == NULL) {
    return;
  }
  if (enable && !g_key_map_overridden) {
    current = IOHIDEventSystemClientCopyProperty(client, CFSTR(kIOHIDUserKeyUsageMapKey));
    existing = (current != NULL && CFGetTypeID(current) == CFArrayGetTypeID()) ? (CFArrayRef)current : NULL;
    count = existing != NULL ? CFArrayGetCount(existing) : 0;
    for (i = 0; i < count; i++) {
      CFTypeRef item = CFArrayGetValueAtIndex(existing, i);
      if (CFGetTypeID(item) == CFDictionaryGetTypeID() && MappingSource((CFDictionaryRef)item) == kVolumeUpUsage) {
        if (current != NULL) {
          CFRelease(current);
        }
        CFRelease(client);
        return;
      }
    }
    g_key_map_saved = existing != NULL ? CFRetain(existing)
                                      : CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
    next = CFArrayCreateMutable(NULL, count + 1, &kCFTypeArrayCallBacks);
    for (i = 0; i < count; i++) {
      CFArrayAppendValue(next, CFArrayGetValueAtIndex(existing, i));
    }
    source = CFNumberCreate(NULL, kCFNumberSInt64Type, &kVolumeUpUsage);
    dest = CFNumberCreate(NULL, kCFNumberSInt64Type, &kF12Usage);
    keys[0] = CFSTR(kIOHIDKeyboardModifierMappingSrcKey);
    keys[1] = CFSTR(kIOHIDKeyboardModifierMappingDstKey);
    values[0] = source;
    values[1] = dest;
    entry = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
    CFArrayAppendValue(next, entry);
    if (IOHIDEventSystemClientSetProperty(client, CFSTR(kIOHIDUserKeyUsageMapKey), next)) {
      g_key_map_overridden = true;
    } else if (g_key_map_saved != NULL) {
      CFRelease(g_key_map_saved);
      g_key_map_saved = NULL;
    }
    CFRelease(entry);
    CFRelease(source);
    CFRelease(dest);
    CFRelease(next);
    if (current != NULL) {
      CFRelease(current);
    }
  } else if (!enable && g_key_map_overridden) {
    IOHIDEventSystemClientSetProperty(client, CFSTR(kIOHIDUserKeyUsageMapKey), g_key_map_saved);
    CFRelease(g_key_map_saved);
    g_key_map_saved = NULL;
    g_key_map_overridden = false;
  }
  CFRelease(client);
}

// The Kinesis F12 key sends Command-Option-D until Fn is on. macOS uses
// that chord to hide the Dock and never delivers it to the window. Hot key
// 52 is that shortcut: D, key code 2, command and option. Turn it off while
// this window is in front, then put it back.
extern CGError CGSSetSymbolicHotKeyEnabled(int hotKey, bool enabled);
extern bool CGSIsSymbolicHotKeyEnabled(int hotKey);
extern CGError CGSGetSymbolicHotKeyValue(int hotKey, uint32_t* character, uint32_t* keycode,
                                        uint32_t* modifiers);

static int g_dock_hotkey = -1;
static bool g_dock_hotkey_was_on = false;

static int DockHidingHotKey(void) {
  int i;
  for (i = 0; i < 256; i++) {
    uint32_t character = 0;
    uint32_t keycode = 0;
    uint32_t modifiers = 0;
    if (CGSGetSymbolicHotKeyValue(i, &character, &keycode, &modifiers) != 0) {
      continue;
    }
    if (keycode == 2 && (modifiers & 0x1E0000u) == 0x180000u) {
      return i;
    }
  }
  return -1;
}

static bool DockShortcutShouldBeOn(int hotkey) {
  CFDictionaryRef all;
  CFStringRef name;
  CFDictionaryRef entry;
  CFTypeRef enabled;
  bool should = true;
  all = CFPreferencesCopyAppValue(CFSTR("AppleSymbolicHotKeys"), CFSTR("com.apple.symbolichotkeys"));
  if (all == NULL || CFGetTypeID(all) != CFDictionaryGetTypeID()) {
    if (all != NULL) {
      CFRelease(all);
    }
    return true;
  }
  name = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), hotkey);
  entry = CFDictionaryGetValue(all, name);
  CFRelease(name);
  if (entry != NULL && CFGetTypeID(entry) == CFDictionaryGetTypeID()) {
    enabled = CFDictionaryGetValue(entry, CFSTR("enabled"));
    if (enabled != NULL && CFGetTypeID(enabled) == CFBooleanGetTypeID() && !CFBooleanGetValue(enabled)) {
      should = false;
    }
  }
  CFRelease(all);
  return should;
}

static void UseDockKeyAsBreak(bool enable) {
  int hotkey;
  if (enable) {
    if (g_dock_hotkey >= 0) {
      return;
    }
    hotkey = DockHidingHotKey();
    if (hotkey < 0 || !DockShortcutShouldBeOn(hotkey)) {
      return;
    }
    g_dock_hotkey = hotkey;
    g_dock_hotkey_was_on = true;
    if (CGSIsSymbolicHotKeyEnabled(hotkey)) {
      CGSSetSymbolicHotKeyEnabled(hotkey, false);
    }
    return;
  }
  if (g_dock_hotkey < 0) {
    return;
  }
  if (g_dock_hotkey_was_on) {
    CGSSetSymbolicHotKeyEnabled(g_dock_hotkey, true);
  }
  g_dock_hotkey = -1;
  g_dock_hotkey_was_on = false;
}

static void UseHostFunctionKeys(bool enable) {
  int mode;
  UseVolumeUpAsBreak(enable);
  if (enable && !g_fkey_overridden) {
    mode = ReadFKeyMode();
    if (mode != 0) {
      return;
    }
    g_fkey_saved = mode;
    WriteFKeyMode(1);
    g_fkey_overridden = true;
  } else if (!enable && g_fkey_overridden) {
    WriteFKeyMode(g_fkey_saved);
    g_fkey_overridden = false;
  }
}

// Put the top row, the volume-up remap, and the Dock shortcut back. Called
// on resign, quit, atexit, and from the signal sources below so a kill from
// the terminal cannot leave the host keyboard stuck on F1-F12.
static void RestoreHostKeyboard(void) {
  UseHostFunctionKeys(false);
  UseDockKeyAsBreak(false);
}

static void InstallKeyboardSignalHandlers(void) {
  static const int kSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
  static dispatch_source_t sources[sizeof(kSignals) / sizeof(kSignals[0])];
  size_t i;
  atexit(RestoreHostKeyboard);
  for (i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); i++) {
    int sig = kSignals[i];
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0,
                               dispatch_get_main_queue());
    if (source == NULL) {
      continue;
    }
    // Deliver the signal to GCD instead of terminating at once.
    signal(sig, SIG_IGN);
    dispatch_source_set_event_handler(source, ^{
      RestoreHostKeyboard();
      [NSApp terminate:nil];
    });
    dispatch_resume(source);
    sources[i] = source;
  }
}

// A keyboard that ignores the switch still sends the picture on the key.
// On an Apple row the last key is volume up, which is where F12 sits.
static bool SpecialIsBreak(int type) { return type == NX_KEYTYPE_SOUND_UP; }

static bool SpecialBbcKey(int type, int* column, int* row) {
  switch (type) {
    case NX_KEYTYPE_BRIGHTNESS_DOWN:
      *column = 1;
      *row = 7;
      return true;
    case NX_KEYTYPE_BRIGHTNESS_UP:
      *column = 2;
      *row = 7;
      return true;
    case NX_KEYTYPE_LAUNCH_PANEL:
      *column = 3;
      *row = 7;
      return true;
    case NX_KEYTYPE_ILLUMINATION_DOWN:
      *column = 4;
      *row = 7;
      return true;
    case NX_KEYTYPE_ILLUMINATION_UP:
      *column = 5;
      *row = 7;
      return true;
    case NX_KEYTYPE_PREVIOUS:
    case NX_KEYTYPE_REWIND:
      *column = 6;
      *row = 1;
      return true;
    case NX_KEYTYPE_PLAY:
      *column = 6;
      *row = 7;
      return true;
    case NX_KEYTYPE_NEXT:
    case NX_KEYTYPE_FAST:
      *column = 7;
      *row = 7;
      return true;
    case NX_KEYTYPE_MUTE:
      *column = 0;
      *row = 2;
      return true;
    default:
      return false;
  }
}

static NSEvent* _Nullable SpecialKeyEvent(NSEvent* event) {
  int type;
  int flags;
  int state;
  int column = 0;
  int row = 0;
  if (!g_ready || event.type != NSEventTypeSystemDefined ||
      event.subtype != NX_SUBTYPE_AUX_CONTROL_BUTTONS) {
    return event;
  }
  type = (int)((event.data1 & 0xFFFF0000) >> 16);
  flags = (int)(event.data1 & 0xFFFF);
  if ((flags & 1) != 0 && (SpecialIsBreak(type) || SpecialBbcKey(type, &column, &row))) {
    return nil;
  }
  state = (flags & 0xFF00) >> 8;
  if (state != 0x0A && state != 0x0B) {
    return event;
  }
  ApplyMomentaryModifiers(event.modifierFlags);
  if (SpecialIsBreak(type)) {
    if (state == 0x0A) {
      W65C02InterpreterResetCpu(&g_cpu);
    }
    return nil;
  }
  if (!SpecialBbcKey(type, &column, &row) || g_cpu.bbc == NULL) {
    return event;
  }
  BbcMachineSetKey(g_cpu.bbc, column, row, state == 0x0A);
  return nil;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  (void)notification;
  // 800x640 is a 5:4 monitor at a size that sits on the desktop.
  // -scale multiplies that. The view draws the bitmap into the whole
  // content area, so the pixels grow by the same amount.
  double scale = BbcSessionScreenScale();
  NSRect content = NSMakeRect(0, 0, 800.0 * scale, 640.0 * scale);
  self.window = [[NSWindow alloc]
      initWithContentRect:content
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.title = @"BBC Micro";
  [self.window setContentAspectRatio:NSMakeSize(5, 4)];
  // 400x320 is the usual smallest size. A scale below 0.5 asks for less
  // than that, so the minimum follows the requested picture.
  if (content.size.width < 400.0) {
    [self.window setContentMinSize:content.size];
  } else {
    [self.window setContentMinSize:NSMakeSize(400, 320)];
  }
  self.view = [[BbcView alloc] initWithFrame:content];
  self.window.contentView = self.view;
  [self.window center];
  [self.window makeKeyAndOrderFront:nil];
  [self.window makeFirstResponder:self.view];
  [self startAudio];
  self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 50.0
                                                target:self
                                              selector:@selector(tick:)
                                              userInfo:nil
                                               repeats:YES];
  [NSApp activateIgnoringOtherApps:YES];
  RememberCapsFlag();
  UseHostFunctionKeys(true);
  UseDockKeyAsBreak(true);
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                       handler:^NSEvent* _Nullable(NSEvent* event) {
                                         bool dock_key =
                                             event.keyCode == 2 &&
                                             (event.modifierFlags & NSEventModifierFlagCommand) != 0 &&
                                             (event.modifierFlags & NSEventModifierFlagOption) != 0;
                                         if (!g_ready || (!dock_key && event.keyCode != 111)) {
                                           return event;
                                         }
                                         if (!event.isARepeat) {
                                           ApplyMomentaryModifiers(event.modifierFlags);
                                           W65C02InterpreterResetCpu(&g_cpu);
                                         }
                                         return nil;
                                       }];
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskSystemDefined
                                       handler:^NSEvent* _Nullable(NSEvent* event) {
                                         return SpecialKeyEvent(event);
                                       }];
}

- (void)applicationDidBecomeActive:(NSNotification*)notification {
  (void)notification;
  UseHostFunctionKeys(true);
  UseDockKeyAsBreak(true);
}

- (void)applicationDidResignActive:(NSNotification*)notification {
  (void)notification;
  ReleaseStuckKeys();
  UseHostFunctionKeys(false);
  UseDockKeyAsBreak(false);
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  (void)notification;
  RestoreHostKeyboard();
  [self.timer invalidate];
  if (self.audioQueue != NULL) {
    AudioQueueStop(self.audioQueue, true);
    AudioQueueDispose(self.audioQueue, true);
    self.audioQueue = NULL;
  }
  g_cpu.running = false;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  (void)sender;
  return YES;
}

@end

static int HostKeyboard(void) {
  TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
  CFStringRef ident;
  int kind = BBC_KEYBOARD_UK;
  if (source == NULL) {
    return kind;
  }
  ident = (CFStringRef)TISGetInputSourceProperty(source, kTISPropertyInputSourceID);
  if (ident != NULL) {
    if (CFStringFind(ident, CFSTR("British"), 0).location != kCFNotFound) {
      kind = BBC_KEYBOARD_UK;
    } else if (CFStringFind(ident, CFSTR("keylayout.US"), 0).location != kCFNotFound ||
               CFStringCompare(ident, CFSTR("com.apple.keylayout.ABC"), 0) == kCFCompareEqualTo) {
      kind = BBC_KEYBOARD_US;
    }
  }
  CFRelease(source);
  return kind;
}

int main(int argc, char** argv) {
  @autoreleasepool {
    int status = 1;
    BbcSessionPreferKeyboard(HostKeyboard());
    if (!BbcSessionStart(argc, argv, &status)) {
      return status;
    }
    InstallKeyboardSignalHandlers();
    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    BbcController* controller = [[BbcController alloc] init];
    app.delegate = controller;
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"BBC Micro"];
    [appMenu addItemWithTitle:@"Quit BBC Micro"
                      action:@selector(terminate:)
               keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    app.mainMenu = bar;
    [app run];
    RestoreHostKeyboard();
    BbcSessionFinish();
  }
  return 0;
}
