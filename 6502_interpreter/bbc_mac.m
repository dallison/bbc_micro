//
//  bbc_mac.m
//  BBC Micro emulator: the 6502 interpreter and BBC hardware, with an
//  AppKit window for the video ULA and Core Audio for the SN76489.
//

#import <AudioToolbox/AudioToolbox.h>
#import <Cocoa/Cocoa.h>

#include "6502_interpreter.h"
#include "bbc_hardware.h"
#include "cassette.h"
#include "cumana.h"

#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>

static const int kFrameCycles = 40000;

static W65C02Interpreter g_cpu;
static bool g_ready = false;

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

// Character the BBC prints for that matrix key. Shifted symbols are the MOS
// bit-paired values (!"#$%&'() and so on), not the legends of a UK PC keyboard.
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
  BbcMachineSetKey(g_cpu.bbc, column, row, true);
  SyncShift();
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
  NSEventModifierFlags flags = event.modifierFlags;
  g_host_shift = (flags & NSEventModifierFlagShift) != 0;
  BbcMachineSetKey(g_cpu.bbc, 1, 0, (flags & NSEventModifierFlagControl) != 0);
  BbcMachineSetKey(g_cpu.bbc, 0, 4, (flags & NSEventModifierFlagCapsLock) != 0);
  SyncShift();
}

@end

@interface BbcController : NSObject <NSApplicationDelegate>
@property(nonatomic) NSWindow* window;
@property(nonatomic) BbcView* view;
@property(nonatomic) NSTimer* timer;
@property(nonatomic) AudioQueueRef audioQueue;
@property(nonatomic) uint8_t* frameCopy;
@property(nonatomic) int sizedWidth;
@property(nonatomic) int sizedHeight;
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
  if (g_cpu.bbc != NULL && self.view != nil && self.window != nil) {
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
  // Real time paints the beam as the CPU runs. Running ahead skips the beam,
  // so snapshot the screen. Otherwise a stretch that never waits, such as the
  // clock face being drawn, keeps showing the previous picture.
  if (g_cpu.turbo || BbcMachineCompletedFrames(g_cpu.bbc) == 0) {
    BbcMachineRender(g_cpu.bbc);
  }
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
  // Pixels are twice as tall as they are wide. Lock that shape so a
  // resize stays looking like a monitor, and only refit when the mode
  // itself changes. The beam height jitters by a line or two.
  if (width != self.sizedWidth || abs(height - self.sizedHeight) > 8) {
    self.sizedWidth = width;
    self.sizedHeight = height;
    [self.window setContentAspectRatio:NSMakeSize(width, height * 2)];
    NSRect frame = self.window.frame;
    NSRect content = [self.window contentRectForFrameRect:frame];
    CGFloat viewH = content.size.width * (CGFloat)(height * 2) / (CGFloat)width;
    CGFloat delta = content.size.height - viewH;
    if (delta < 0) {
      delta = -delta;
    }
    if (delta > 1.0) {
      NSRect next =
          [self.window frameRectForContentRect:NSMakeRect(0, 0, content.size.width, viewH)];
      CGFloat top = frame.origin.y + frame.size.height;
      next.origin.x = frame.origin.x;
      next.origin.y = top - next.size.height;
      [self.window setFrame:next display:NO];
    }
  }
  [self.view setNeedsDisplay:YES];
}

// macOS uses the top row as brightness and volume unless this preference is
// on. Remember the user's value and turn the row into F1-F12 while the
// emulator is the front application, then put the old value back.
static bool g_fn_known = false;
static bool g_fn_existed = false;
static bool g_fn_was_on = false;
static bool g_fn_overridden = false;

static void RememberFnState(void) {
  CFPropertyListRef value;
  if (g_fn_known) {
    return;
  }
  value = CFPreferencesCopyAppValue(CFSTR("com.apple.keyboard.fnState"),
                                    kCFPreferencesAnyApplication);
  g_fn_known = true;
  if (value == NULL) {
    return;
  }
  g_fn_existed = true;
  if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
    g_fn_was_on = CFBooleanGetValue((CFBooleanRef)value);
  } else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
    int number = 0;
    CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &number);
    g_fn_was_on = number != 0;
  }
  CFRelease(value);
}

static void StoreFnState(CFPropertyListRef value) {
  NSString* tool =
      @"/System/Library/PrivateFrameworks/SystemAdministration.framework/Resources/activateSettings";
  CFPreferencesSetValue(CFSTR("com.apple.keyboard.fnState"), value, kCFPreferencesAnyApplication,
                        kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
  CFPreferencesSynchronize(kCFPreferencesAnyApplication, kCFPreferencesCurrentUser,
                           kCFPreferencesAnyHost);
  if (![[NSFileManager defaultManager] isExecutableFileAtPath:tool]) {
    return;
  }
  NSTask* task = [[NSTask alloc] init];
  NSError* error = nil;
  task.executableURL = [NSURL fileURLWithPath:tool];
  task.arguments = @[@"-u"];
  task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
  task.standardError = [NSFileHandle fileHandleWithNullDevice];
  if ([task launchAndReturnError:&error]) {
    [task waitUntilExit];
  }
}

static void UseHostFunctionKeys(bool enable) {
  RememberFnState();
  if (g_fn_was_on) {
    return;
  }
  if (enable && !g_fn_overridden) {
    StoreFnState(kCFBooleanTrue);
    g_fn_overridden = true;
  } else if (!enable && g_fn_overridden) {
    StoreFnState(g_fn_existed ? (CFPropertyListRef)(g_fn_was_on ? kCFBooleanTrue : kCFBooleanFalse)
                             : NULL);
    g_fn_overridden = false;
  }
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  (void)notification;
  // 800x640 is a 5:4 monitor at a size that sits on the desktop.
  NSRect content = NSMakeRect(0, 0, 800, 640);
  self.window = [[NSWindow alloc]
      initWithContentRect:content
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.title = @"BBC Micro";
  [self.window setContentAspectRatio:NSMakeSize(5, 4)];
  [self.window setContentMinSize:NSMakeSize(400, 320)];
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
  UseHostFunctionKeys(true);
}

- (void)applicationDidBecomeActive:(NSNotification*)notification {
  (void)notification;
  UseHostFunctionKeys(true);
}

- (void)applicationDidResignActive:(NSNotification*)notification {
  (void)notification;
  UseHostFunctionKeys(false);
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  (void)notification;
  UseHostFunctionKeys(false);
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

static void Usage(void) {
  fprintf(stderr,
          "usage: bbc [-machine b|master|master128|master256] [-sram spec]\n"
          "           [-os file] [-rom slot,file] [-roms dir] [-mode 0-7]\n"
          "           [-disc file] [-disc0 file] [-disc1 file] [-disc-port n]\n"
          "           [-fdc 8271|1770] [-fs dfs|adfs] [-65c02]\n"
          "           [-tape file] [-tape-port n]\n"
          "           [-printer file] [-mhz n] [-turbo]\n"
          "       The default machine is the Model B. Its ROMs come from\n"
          "       bbc_b_rom_sockets. master and master128 use\n"
          "       bbc_master_rom_sockets, a 65SC12, a WD1770, and sideways RAM\n"
          "       in sockets 4-7. master256 makes every sideways socket RAM.\n"
          "       -sram 4, 8, 16, or a list such as 4-7 replaces that set.\n"
          "       os.rom or os-<name>.rom is the OS. <socket>-<name>.rom is\n"
          "       sideways socket 0-15. -os and -rom replace one of those files.\n"
          "       The Model B CPU is the NMOS 6502. -65c02 keeps the CMOS opcodes\n"
          "       and the davecc $EF syscall. F1 to F9 are the BBC keys f1 to f9,\n"
          "       and F10 is f0. F12 resets the machine. While this window is in\n"
          "       front, that row is the BBC keys.\n"
          "       DFS and ADFS ROMs are both fitted. The drive follows the one\n"
          "       the OS calls. -fs dfs or -fs adfs loads only that filing\n"
          "       system.\n"
          "       cumana inserts a disc while the machine is running. It\n"
          "       connects to 127.0.0.1:8177. -disc-port chooses another port,\n"
          "       and -disc-port 0 closes the socket. -disc still mounts an\n"
          "       image before startup.\n"
          "       cassette inserts a tape and records guest saves back into\n"
          "       the file. It connects to 127.0.0.1:8178. -tape-port 0 closes\n"
          "       that socket. -tape still loads a tape before startup.\n"
          "       The CPU runs at 2 MHz, in step with the timers and the video.\n"
          "       -mhz n selects another fixed rate, from 1 to 16. -2mhz is\n"
          "       the same as -mhz 2. -turbo lets a busy program run ahead\n"
          "       of the wall clock.\n");
}

int main(int argc, char** argv) {
  @autoreleasepool {
    const char* os_path = NULL;
    int mode = -1;
    bool selftest = false;
    int rom_slots[16];
    const char* rom_paths[16];
    int rom_count = 0;
    const char* disc_paths[2] = {NULL, NULL};
    int disc_next = 0;
    int disc_port = CUMANA_PORT;
    bool disc_socket = true;
    int tape_port = CASSETTE_PORT;
    bool tape_socket = true;
    int fdc_kind = 0;
    int filing = BBC_FS_ANY;
    bool use_65c02 = false;
    int clock_mhz = 2;
    bool run_ahead = false;
    const char* tape_path = NULL;
    const char* printer_path = NULL;
    const char* roms_arg = NULL;
    int machine = BBC_MACHINE_B;
    bool sram_set = false;
    bool sram[16];
    char* rom_dir = NULL;
    BbcRomFile rom_files[BBC_ROM_IMAGE_MAX];
    int rom_file_count = 0;
    bool skip_slot[16];
    int i;
    memset(skip_slot, 0, sizeof(skip_slot));
    for (i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-os") == 0 || strcmp(argv[i], "-bbc-os") == 0) {
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        os_path = argv[++i];
      } else if (strcmp(argv[i], "-mode") == 0 || strcmp(argv[i], "-bbc-mode") == 0) {
        if (i + 1 >= argc || argv[i + 1][0] < '0' || argv[i + 1][0] > '7' ||
            argv[i + 1][1] != '\0') {
          Usage();
          return 1;
        }
        mode = argv[++i][0] - '0';
      } else if (strcmp(argv[i], "-rom") == 0 || strcmp(argv[i], "-bbc-rom") == 0) {
        const char* spec;
        const char* comma;
        char slot_text[8];
        size_t slot_len;
        int slot;
        if (i + 1 >= argc || rom_count == 16) {
          Usage();
          return 1;
        }
        spec = argv[++i];
        comma = strchr(spec, ',');
        if (comma == NULL || comma == spec || comma[1] == '\0') {
          Usage();
          return 1;
        }
        slot_len = (size_t)(comma - spec);
        if (slot_len >= sizeof(slot_text)) {
          Usage();
          return 1;
        }
        memcpy(slot_text, spec, slot_len);
        slot_text[slot_len] = '\0';
        slot = atoi(slot_text);
        if (slot < 0 || slot > 15) {
          Usage();
          return 1;
        }
        rom_slots[rom_count] = slot;
        rom_paths[rom_count] = comma + 1;
        rom_count++;
      } else if (strcmp(argv[i], "-disc") == 0 || strcmp(argv[i], "-disc0") == 0 ||
                 strcmp(argv[i], "-disc1") == 0) {
        int drive;
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        if (strcmp(argv[i], "-disc1") == 0) {
          drive = 1;
        } else if (strcmp(argv[i], "-disc0") == 0) {
          drive = 0;
        } else {
          drive = disc_next < 2 ? disc_next : 1;
        }
        disc_paths[drive] = argv[++i];
        if (drive >= disc_next && disc_next < 2) {
          disc_next = drive + 1;
        }
      } else if (strcmp(argv[i], "-disc-port") == 0) {
        char* end = NULL;
        long value;
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        value = strtol(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value < 0 || value > 65535) {
          Usage();
          return 1;
        }
        if (value == 0) {
          disc_socket = false;
        } else {
          disc_port = (int)value;
        }
      } else if (strcmp(argv[i], "-tape-port") == 0 ||
                 strcmp(argv[i], "-bbc-tape-port") == 0) {
        char* end = NULL;
        long value;
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        value = strtol(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value < 0 || value > 65535) {
          Usage();
          return 1;
        }
        if (value == 0) {
          tape_socket = false;
        } else {
          tape_port = (int)value;
        }
      } else if (strcmp(argv[i], "-fs") == 0 || strcmp(argv[i], "-bbc-fs") == 0) {
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        filing = BbcMachineParseFilingSystem(argv[++i]);
        if (filing < 0) {
          Usage();
          return 1;
        }
      } else if (strcmp(argv[i], "-fdc") == 0) {
        const char* which;
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        which = argv[++i];
        if (strcmp(which, "8271") == 0) {
          fdc_kind = BBC_FDC_8271;
        } else if (strcmp(which, "1770") == 0) {
          fdc_kind = BBC_FDC_1770;
        } else {
          Usage();
          return 1;
        }
      } else if (strcmp(argv[i], "-machine") == 0 || strcmp(argv[i], "-bbc-machine") == 0) {
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        machine = BbcMachineParseModel(argv[++i]);
        if (machine < 0) {
          Usage();
          return 1;
        }
      } else if (strcmp(argv[i], "-sram") == 0 || strcmp(argv[i], "-bbc-sram") == 0) {
        if (i + 1 >= argc || !BbcMachineParseSidewaysRam(argv[++i], sram)) {
          Usage();
          return 1;
        }
        sram_set = true;
      } else if (strcmp(argv[i], "-roms") == 0 || strcmp(argv[i], "-bbc-roms") == 0) {
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        roms_arg = argv[++i];
      } else if (strcmp(argv[i], "-65c02") == 0 || strcmp(argv[i], "-bbc-65c02") == 0) {
        use_65c02 = true;
      } else if (strcmp(argv[i], "-tape") == 0 || strcmp(argv[i], "-bbc-tape") == 0) {
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        tape_path = argv[++i];
      } else if (strcmp(argv[i], "-printer") == 0 || strcmp(argv[i], "-bbc-printer") == 0) {
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        printer_path = argv[++i];
      } else if (strcmp(argv[i], "-2mhz") == 0) {
        clock_mhz = 2;
        run_ahead = false;
      } else if (strcmp(argv[i], "-turbo") == 0) {
        run_ahead = true;
      } else if (strcmp(argv[i], "-mhz") == 0) {
        char* end = NULL;
        long value;
        if (i + 1 >= argc) {
          Usage();
          return 1;
        }
        value = strtol(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' ||
            !W65C02InterpreterSetClockMhz(&g_cpu, (int)value)) {
          Usage();
          return 1;
        }
        clock_mhz = (int)value;
        run_ahead = false;
      } else if (strcmp(argv[i], "-selftest") == 0) {
        selftest = true;
      } else if (strcmp(argv[i], "-h") == 0) {
        Usage();
        return 0;
      } else {
        Usage();
        return 1;
      }
    }

    if (roms_arg != NULL) {
      rom_file_count = BbcMachineListRomDirectory(roms_arg, rom_files, BBC_ROM_IMAGE_MAX, filing);
      if (rom_file_count < 0) {
        return 1;
      }
    } else if (!selftest) {
      rom_dir = BbcMachineFindRomDirectory(argv[0], BbcMachineRomDirectoryName(machine));
      if (rom_dir != NULL) {
        rom_file_count = BbcMachineListRomDirectory(rom_dir, rom_files, BBC_ROM_IMAGE_MAX, filing);
        if (rom_file_count < 0) {
          free(rom_dir);
          return 1;
        }
      }
    }
    if (os_path == NULL) {
      os_path = BbcRomOsPath(rom_files, rom_file_count);
      if (os_path != NULL) {
        fprintf(stderr, "ROM os: %s\n", os_path);
      }
    }
    W65C02InterpreterInit(&g_cpu, false, true, false, NULL);
    if (!run_ahead && !W65C02InterpreterSetClockMhz(&g_cpu, clock_mhz)) {
      Usage();
      return 1;
    }
    W65C02InterpreterUseBbc(&g_cpu, mode, NULL, os_path);
    if (g_cpu.bbc == NULL || !BbcMachineSetModel(g_cpu.bbc, machine)) {
      fprintf(stderr, "Unable to select the BBC machine\n");
      return 1;
    }
    if (sram_set && !BbcMachineSetSidewaysRam(g_cpu.bbc, sram)) {
      fprintf(stderr, "Unable to allocate sideways RAM\n");
      return 1;
    }
    for (i = 0; i < rom_count; i++) {
      if (!W65C02InterpreterBbcLoadRom(&g_cpu, rom_slots[i], rom_paths[i])) {
        BbcRomFileFree(rom_files, rom_file_count);
        free(rom_dir);
        return 1;
      }
      skip_slot[rom_slots[i]] = true;
    }
    if (!BbcMachineLoadRomFiles(g_cpu.bbc, rom_files, rom_file_count, skip_slot)) {
      BbcRomFileFree(rom_files, rom_file_count);
      free(rom_dir);
      return 1;
    }
    for (i = 0; i < 2; i++) {
      if (disc_paths[i] != NULL &&
          !W65C02InterpreterBbcLoadDisc(&g_cpu, i, disc_paths[i])) {
        return 1;
      }
    }
    if (disc_socket) {
      int bound = BbcMachineListenDiscs(g_cpu.bbc, disc_port);
      if (bound < 0) {
        fprintf(stderr, "Disc socket is not available\n");
      } else {
        fprintf(stderr, "Disc socket: 127.0.0.1:%d\n", bound);
      }
    }
    if (tape_socket) {
      int bound = BbcMachineListenTapes(g_cpu.bbc, tape_port);
      if (bound < 0) {
        fprintf(stderr, "Tape socket is not available\n");
      } else {
        fprintf(stderr, "Tape socket: 127.0.0.1:%d\n", bound);
      }
    }
    if (fdc_kind != 0) {
      W65C02InterpreterBbcSetFdc(&g_cpu, fdc_kind);
    } else if (filing != BBC_FS_ANY) {
      W65C02InterpreterBbcSetFdc(&g_cpu, BbcMachineControllerForFiling(g_cpu.bbc, filing));
    } else if (BbcMachineIsMaster(g_cpu.bbc)) {
      W65C02InterpreterBbcSetFdc(&g_cpu, BBC_FDC_1770);
    }
    if (use_65c02 || BbcMachineIsMaster(g_cpu.bbc)) {
      W65C02InterpreterBbcUse65C02(&g_cpu, true);
    }
    if (tape_path != NULL && !BbcMachineLoadTape(g_cpu.bbc, tape_path)) {
      fprintf(stderr, "Unable to load tape %s\n", tape_path);
      return 1;
    }
    if (printer_path != NULL) {
      BbcMachineSetPrinter(g_cpu.bbc, printer_path);
    }
    if (!W65C02InterpreterPrepareBbc(&g_cpu)) {
      fprintf(stderr, "Unable to start the BBC Micro\n");
      return 1;
    }
    g_ready = true;
    if (selftest) {
      int cycles = 0;
      int guard = 0;
      int lit = 0;
      int energy = 0;
      int n;
      int s;
      int16_t audio[4096];
      const uint8_t* fb;
      while (cycles < 120000 && guard < 1000000) {
        int step = W65C02InterpreterStep(&g_cpu);
        if (step <= 0) {
          break;
        }
        cycles += step;
        guard++;
      }
      if (BbcMachineCompletedFrames(g_cpu.bbc) == 0) {
        BbcMachineRender(g_cpu.bbc);
      }
      fb = BbcFramebuffer(g_cpu.bbc);
      for (s = 0; s < BBC_FB_WIDTH * BbcFrameHeight(g_cpu.bbc) * 3; s++) {
        if (fb[s] != 0) {
          lit++;
        }
      }
      n = BbcMachineReadAudio(g_cpu.bbc, audio, 4096);
      for (s = 0; s < n; s++) {
        energy += audio[s] < 0 ? -audio[s] : audio[s];
      }
      fprintf(stderr, "selftest cycles=%d lit=%d audio=%d energy=%d\n", cycles, lit, n, energy);
      W65C02InterpreterDestruct(&g_cpu);
      BbcRomFileFree(rom_files, rom_file_count);
      free(rom_dir);
      return (lit > 100 && energy > 100000) ? 0 : 1;
    }

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
    W65C02InterpreterDestruct(&g_cpu);
    BbcRomFileFree(rom_files, rom_file_count);
    free(rom_dir);
  }
  return 0;
}
