//
//  joystick_host_mac.m
//  Mouse-driven analogue joystick for a running bbc emulator.
//

#import <AppKit/AppKit.h>
#include "joystick.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fd = -1;
static uint8_t g_digital = 0;
static const char* g_host = "127.0.0.1";
static int g_port = JOYSTICK_PORT;

@interface JoystickView : NSView
@end

@implementation JoystickView

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  (void)event;
  return YES;
}

- (void)sendPacketWithX:(uint16_t)x y:(uint16_t)y fire0:(bool)f0 fire1:(bool)f1 {
  JoystickPacket packet;
  if (g_fd < 0) {
    return;
  }
  packet.x = x;
  packet.y = y;
  packet.fire0 = f0 ? 1 : 0;
  packet.fire1 = f1 ? 1 : 0;
  packet.digital = g_digital;
  packet.pad = 0;
  if (JoystickSendPacket(g_fd, &packet) != 0) {
    SocketClose(g_fd);
    g_fd = -1;
    dispatch_async(dispatch_get_main_queue(), ^{
      NSAlert* alert = [[NSAlert alloc] init];
      alert.messageText = @"Lost connection to the BBC Micro";
      alert.informativeText = @"Start bbc first, then run joystick again.";
      [alert runModal];
      [NSApp terminate:nil];
    });
  }
}

- (void)tick:(NSTimer*)timer {
  (void)timer;
  NSPoint loc = [self convertPoint:[self.window mouseLocationOutsideOfEventStream] fromView:nil];
  NSRect bounds = self.bounds;
  uint16_t ax = 0x8000;
  uint16_t ay = 0x8000;
  bool inside = NSPointInRect(loc, bounds);
  if (inside && bounds.size.width > 1.0 && bounds.size.height > 1.0) {
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
    ax = (uint16_t)(nx * 65535.0);
    ay = (uint16_t)((1.0 - ny) * 65535.0);
  }
  NSUInteger buttons = [NSEvent pressedMouseButtons];
  [self sendPacketWithX:ax y:ay fire0:(buttons & 1) != 0 fire1:(buttons & 2) != 0];
}

- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;
  NSRect bounds = self.bounds;
  [[NSColor windowBackgroundColor] setFill];
  NSRectFill(bounds);
  [[NSColor secondaryLabelColor] set];
  NSString* hint =
      @"Move the mouse here for X/Y.\n"
      @"Left click: fire 1. Right click: fire 2.\n"
      @"Arrow keys or WASD: digital stick.\n"
      @"In Revs: Shift+F2 selects the joystick.";
  [hint drawInRect:NSInsetRect(bounds, 16, 16)
      withAttributes:@{
        NSFontAttributeName : [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName : [NSColor secondaryLabelColor],
      }];
  [[NSColor separatorColor] setStroke];
  NSBezierPath* cross = [NSBezierPath bezierPath];
  [cross moveToPoint:NSMakePoint(NSMidX(bounds), 8)];
  [cross lineToPoint:NSMakePoint(NSMidX(bounds), bounds.size.height - 8)];
  [cross moveToPoint:NSMakePoint(8, NSMidY(bounds))];
  [cross lineToPoint:NSMakePoint(bounds.size.width - 8, NSMidY(bounds))];
  [cross stroke];
}

- (void)keyDown:(NSEvent*)event {
  unsigned short code = event.keyCode;
  switch (code) {
    case 123:
      g_digital |= JOYSTICK_DIGITAL_LEFT;
      break;
    case 124:
      g_digital |= JOYSTICK_DIGITAL_RIGHT;
      break;
    case 125:
      g_digital |= JOYSTICK_DIGITAL_DOWN;
      break;
    case 126:
      g_digital |= JOYSTICK_DIGITAL_UP;
      break;
    case 0:
      g_digital |= JOYSTICK_DIGITAL_LEFT;
      break;
    case 2:
      g_digital |= JOYSTICK_DIGITAL_RIGHT;
      break;
    case 1:
      g_digital |= JOYSTICK_DIGITAL_DOWN;
      break;
    case 13:
      g_digital |= JOYSTICK_DIGITAL_UP;
      break;
    default:
      [super keyDown:event];
      return;
  }
  [self setNeedsDisplay:YES];
}

- (void)keyUp:(NSEvent*)event {
  unsigned short code = event.keyCode;
  switch (code) {
    case 123:
      g_digital &= (uint8_t)~JOYSTICK_DIGITAL_LEFT;
      break;
    case 124:
      g_digital &= (uint8_t)~JOYSTICK_DIGITAL_RIGHT;
      break;
    case 125:
      g_digital &= (uint8_t)~JOYSTICK_DIGITAL_DOWN;
      break;
    case 126:
      g_digital &= (uint8_t)~JOYSTICK_DIGITAL_UP;
      break;
    case 0:
      g_digital &= (uint8_t)~JOYSTICK_DIGITAL_LEFT;
      break;
    case 2:
      g_digital &= (uint8_t)~JOYSTICK_DIGITAL_RIGHT;
      break;
    case 1:
      g_digital &= (uint8_t)~JOYSTICK_DIGITAL_DOWN;
      break;
    case 13:
      g_digital &= (uint8_t)~JOYSTICK_DIGITAL_UP;
      break;
    default:
      [super keyUp:event];
      return;
  }
  [self setNeedsDisplay:YES];
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

@end

@interface JoystickAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic) NSWindow* window;
@end

@implementation JoystickAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  (void)note;
  const char* hostEnv = getenv("BBC_JOYSTICK_HOST");
  const char* portEnv = getenv("BBC_JOYSTICK_PORT");
  const char* host = g_host;
  int port = g_port;
  if (portEnv != NULL && portEnv[0] != '\0') {
    port = (int)strtol(portEnv, NULL, 10);
  }
  if (hostEnv != NULL && hostEnv[0] != '\0') {
    host = hostEnv;
  }
  g_fd = JoystickConnect(host, port);
  if (g_fd < 0) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Could not connect to the BBC Micro";
    alert.informativeText = [NSString stringWithFormat:@"Nothing is listening on %s:%d. "
                                                       @"Start bbc first (joystick socket is on by default).",
                                                       host, port];
    [alert runModal];
    [NSApp terminate:nil];
    return;
  }
  JoystickView* view = [[JoystickView alloc] initWithFrame:NSMakeRect(0, 0, 360, 360)];
  self.window = [[NSWindow alloc]
      initWithContentRect:view.bounds
                styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable)
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.title = @"BBC Joystick";
  self.window.contentView = view;
  self.window.releasedWhenClosed = NO;
  [self.window makeKeyAndOrderFront:nil];
  [self.window makeFirstResponder:view];
  [NSApp activateIgnoringOtherApps:YES];
  [NSTimer scheduledTimerWithTimeInterval:1.0 / 50.0
                                   target:view
                                 selector:@selector(tick:)
                                 userInfo:nil
                                  repeats:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  (void)sender;
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
  (void)note;
  if (g_fd >= 0) {
    SocketClose(g_fd);
    g_fd = -1;
  }
}

@end

int main(int argc, const char* argv[]) {
  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-host") == 0 && i + 1 < argc) {
      g_host = argv[++i];
    } else if (strcmp(argv[i], "-port") == 0 && i + 1 < argc) {
      g_port = (int)strtol(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      fprintf(stderr, "usage: joystick [-host addr] [-port n]\n");
      fprintf(stderr, "       Connects to a running bbc on 127.0.0.1:%d by default.\n", JOYSTICK_PORT);
      return 0;
    }
  }
  @autoreleasepool {
    JoystickAppDelegate* delegate = [[JoystickAppDelegate alloc] init];
    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
