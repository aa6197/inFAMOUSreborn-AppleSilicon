// mac_kbm.mm - keyboard + mouse controls for the macOS app.
//
// cellPad.c runs the same keyboard + mouse pad as Windows (pad_poll_kbm:
// WASD move, mouse look, Space jump, left click shock bolt, ...). On Windows
// it reads the keys and mouse itself. Here AppKit event monitors report them
// (pad_host_kbm.h) and this file hides and holds the pointer while the game
// wants mouse look, as the Windows build does.
//
// A connected controller still owns port 0: cellPad.c merges the keys in and
// takes the sticks from the keyboard or mouse only while they move.
//
// Main thread only (installed by port_mac_run_game before [NSApp run]).
//
// Env:
//   PAD_KBM=0 or PAD_NO_KEYBOARD=1  no keyboard + mouse (controller only)
//   INFAMOUS_KBM_TEST=1             tests: input posted to this process
//                                   (CGEvent postToPid) counts even when the
//                                   app is in the background, and the real
//                                   pointer is never held

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>   // kVK_* key codes (HIToolbox/Events.h, header only)
#import <CoreGraphics/CoreGraphics.h>
#include <cmath>
#include <cstdlib>

#include "pad_host_kbm.h"

static bool s_test = false;
static bool s_focused = false;
static bool s_captured = false;   // pointer hidden and held
static double s_rem_x = 0.0, s_rem_y = 0.0;   // sub-point mouse movement carried to the next event

// The Metal backend's window: its content view's layer is a CAMetalLayer.
static bool is_game_window(NSWindow* w)
{
    static Class metal_layer = NSClassFromString(@"CAMetalLayer");
    return w && metal_layer && [w.contentView.layer isKindOfClass:metal_layer];
}

static NSWindow* game_window(void)
{
    for (NSWindow* w in NSApp.windows)
        if (is_game_window(w)) return w;
    return nil;
}

// Mac key code (physical key, US layout position) -> Windows virtual key.
// Physical positions keep WASD under the left hand on any layout.
static int vk_for_keycode(unsigned short kc)
{
    switch (kc) {
    case kVK_ANSI_A: return 'A'; case kVK_ANSI_B: return 'B'; case kVK_ANSI_C: return 'C';
    case kVK_ANSI_D: return 'D'; case kVK_ANSI_E: return 'E'; case kVK_ANSI_F: return 'F';
    case kVK_ANSI_G: return 'G'; case kVK_ANSI_H: return 'H'; case kVK_ANSI_I: return 'I';
    case kVK_ANSI_J: return 'J'; case kVK_ANSI_K: return 'K'; case kVK_ANSI_L: return 'L';
    case kVK_ANSI_M: return 'M'; case kVK_ANSI_N: return 'N'; case kVK_ANSI_O: return 'O';
    case kVK_ANSI_P: return 'P'; case kVK_ANSI_Q: return 'Q'; case kVK_ANSI_R: return 'R';
    case kVK_ANSI_S: return 'S'; case kVK_ANSI_T: return 'T'; case kVK_ANSI_U: return 'U';
    case kVK_ANSI_V: return 'V'; case kVK_ANSI_W: return 'W'; case kVK_ANSI_X: return 'X';
    case kVK_ANSI_Y: return 'Y'; case kVK_ANSI_Z: return 'Z';
    case kVK_Return: case kVK_ANSI_KeypadEnter: return PAD_VK_RETURN;
    case kVK_Tab:        return PAD_VK_TAB;
    case kVK_Space:      return PAD_VK_SPACE;
    case kVK_Delete:     return PAD_VK_BACK;     // the Mac "delete" key is Backspace
    case kVK_Escape:     return PAD_VK_ESCAPE;
    case kVK_LeftArrow:  return PAD_VK_LEFT;
    case kVK_RightArrow: return PAD_VK_RIGHT;
    case kVK_UpArrow:    return PAD_VK_UP;
    case kVK_DownArrow:  return PAD_VK_DOWN;
    default: return 0;
    }
}

static void set_capture(bool on, NSWindow* w)
{
    if (on == s_captured) return;
    s_captured = on;
    if (on) {
        // Park the pointer in the middle of the game view (CG: top-left origin of the main display).
        const NSRect r = [w contentRectForFrameRect:w.frame];
        const CGFloat main_h = NSScreen.screens.count ? NSScreen.screens[0].frame.size.height : 0;
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(r), main_h - NSMidY(r)));
        CGAssociateMouseAndMouseCursorPosition(false);
        [NSCursor hide];
    } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
    }
}

// 60 times a second: focus (keys count only while the game window is the key window of the active app) and the
// pointer hold that cellPad.c asks for.
static void tick(void)
{
    NSWindow* w = game_window();
    if (w && !w.acceptsMouseMovedEvents) w.acceptsMouseMovedEvents = YES;
    const bool focused = s_test || (w && NSApp.isActive && w.isKeyWindow && !w.isMiniaturized);
    if (focused != s_focused) {
        s_focused = focused;
        s_rem_x = s_rem_y = 0.0;
        ps3_pad_host_focus(focused ? 1 : 0);
    }
    set_capture(!s_test && focused && w && ps3_pad_host_mouse_wanted(), w);
}

// A left click counts only inside the game view (as the Windows build's client-area test), so clicks on the title
// bar or a resize edge do not fire.
static bool click_in_game_view(NSEvent* e)
{
    if (s_test || s_captured) return true;
    NSWindow* w = e.window;
    if (!is_game_window(w)) return false;
    NSView* v = w.contentView;
    return NSPointInRect([v convertPoint:e.locationInWindow fromView:nil], v.bounds);
}

static int vk_for_button(NSInteger b)
{
    return b == 0 ? PAD_VK_LBUTTON : b == 1 ? PAD_VK_RBUTTON : b == 2 ? PAD_VK_MBUTTON : 0;
}

static NSEvent* on_event(NSEvent* e)
{
    switch (e.type) {
    case NSEventTypeKeyDown:
    case NSEventTypeKeyUp: {
        const bool down = e.type == NSEventTypeKeyDown;
        const int vk = vk_for_keycode(e.keyCode);
        if (!down) {   // always release, even with Cmd held or focus elsewhere (no stuck keys)
            if (vk) ps3_pad_host_key(vk, 0);
            return s_focused ? nil : e;
        }
        if (e.modifierFlags & NSEventModifierFlagCommand) return e;   // Cmd+Q, Cmd+H, Ctrl+Cmd+F ...
        if (!s_focused) return e;
        if (vk && !e.isARepeat) ps3_pad_host_key(vk, 1);
        return nil;   // the game view takes no text: swallowing keeps AppKit from beeping
    }
    case NSEventTypeFlagsChanged: {
        const NSEventModifierFlags f = e.modifierFlags;
        const bool on = s_focused;
        ps3_pad_host_key(PAD_VK_SHIFT,   on && (f & NSEventModifierFlagShift));
        ps3_pad_host_key(PAD_VK_CONTROL, on && (f & NSEventModifierFlagControl));
        ps3_pad_host_key(PAD_VK_MENU,    on && (f & NSEventModifierFlagOption));
        return e;
    }
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown: {
        const int vk = vk_for_button(e.buttonNumber);
        if (vk && s_focused && click_in_game_view(e)) ps3_pad_host_key(vk, 1);
        return e;
    }
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp: {
        const int vk = vk_for_button(e.buttonNumber);
        if (vk) ps3_pad_host_key(vk, 0);
        return e;
    }
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged: {
        if (!s_focused) return e;
        s_rem_x += e.deltaX;   // points, x right, y down (as Windows mouse counts)
        s_rem_y += e.deltaY;
        const long dx = (long)std::trunc(s_rem_x), dy = (long)std::trunc(s_rem_y);
        s_rem_x -= (double)dx; s_rem_y -= (double)dy;
        if (dx || dy) ps3_pad_host_mouse_move(dx, dy);
        return e;
    }
    case NSEventTypeScrollWheel: {
        if (!s_focused) return e;
        double dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 10.0 : e.scrollingDeltaY;
        if (e.isDirectionInvertedFromDevice) dy = -dy;   // + = wheel turned away from the player
        if (dy != 0.0) ps3_pad_host_wheel((long)std::lround(dy * 120.0));   // 120 = one notch (Windows WHEEL_DELTA)
        return e;
    }
    default:
        return e;
    }
}

extern "C" void port_mac_kbm_install(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    if (!ps3_pad_host_kbm_enabled()) {
        fprintf(stderr, "[mac] keyboard + mouse off (PAD_KBM=0 or PAD_NO_KEYBOARD)\n");
        return;
    }
    const char* t = getenv("INFAMOUS_KBM_TEST");
    s_test = t && *t && *t != '0';
    const NSEventMask mask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged |
        NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
        NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp | NSEventMaskMouseMoved |
        NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
        NSEventMaskScrollWheel;
    [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* e) { return on_event(e); }];
    NSTimer* timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer*) { tick(); }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    // Hand the pointer back when the app quits.
    [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationWillTerminateNotification object:nil
        queue:nil usingBlock:^(NSNotification*) { set_capture(false, nil); }];
    fprintf(stderr, "[mac] keyboard + mouse on%s: WASD move, mouse look, Space jump, Esc pause/free the pointer\n",
            s_test ? " (test mode)" : "");
}
