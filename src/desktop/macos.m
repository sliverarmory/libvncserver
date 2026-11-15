/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <limits.h>
#include <stdlib.h>
#include <rfb/keysym.h>
#include "backend.h"

/* ScreenCaptureKit's screenshot API requires macOS 14. The asynchronous
 * completion handlers never touch the C backend or its framebuffer. */
@interface VNCDesktopCaptureResult : NSObject {
@public
    dispatch_semaphore_t ready;
    SCShareableContent *content;
    NSError *error;
    CGImageRef image;
}
@end

@implementation VNCDesktopCaptureResult
- (instancetype)init
{
    if ((self = [super init]))
        ready = dispatch_semaphore_create(0);
    return self;
}
- (void)dealloc
{
    if (image)
        CGImageRelease(image);
}
@end

@interface VNCDesktopCaptureState : NSObject
@property(strong) SCContentFilter *filter;
@property(strong) SCStreamConfiguration *configuration;
@end
@implementation VNCDesktopCaptureState
@end

struct desktop_backend {
    void *state; /* Retained VNCDesktopCaptureState. */
    CGDirectDisplayID display;
    CGEventSourceRef source;
    unsigned char *pixels;
    int width, height;
    int buttons;
    CGPoint pointer;
    CGPoint last_click[3];
    CFAbsoluteTime last_click_time[3];
    int click_count[3];
    unsigned char keys[128];
    CGEventFlags flags;
};

static int capture_error(void)
{
    return CGPreflightScreenCaptureAccess() ? VNCDESKTOP_CAPTURE_FAILED
                                            : VNCDESKTOP_PERMISSION_DENIED;
}

int desktop_open(desktop_backend **result)
{
    *result = NULL;
    @autoreleasepool {
        /* Do not trigger consent dialogs from a library or try to circumvent
         * the host application's privacy settings. */
        if (!CGPreflightScreenCaptureAccess() || !AXIsProcessTrusted())
            return VNCDESKTOP_PERMISSION_DENIED;
        CGDirectDisplayID display = CGMainDisplayID();
        if (!display || !CGDisplayIsActive(display))
            return VNCDESKTOP_NO_DESKTOP;

        VNCDesktopCaptureResult *request = [VNCDesktopCaptureResult new];
        [SCShareableContent getShareableContentExcludingDesktopWindows:NO
            onScreenWindowsOnly:YES completionHandler:^(SCShareableContent *content,
                                                        NSError *error) {
                request->content = content;
                request->error = error;
                dispatch_semaphore_signal(request->ready);
            }];
        /* Wait for completion before returning: a timed-out request could
         * otherwise outlive a caller unloading this shared library. These
         * callbacks run on ScreenCaptureKit's queue, not the worker thread. */
        dispatch_semaphore_wait(request->ready, DISPATCH_TIME_FOREVER);
        if (request->error || !request->content)
            return capture_error();

        SCDisplay *selected = nil;
        for (SCDisplay *candidate in request->content.displays)
            if (candidate.displayID == display) {
                selected = candidate;
                break;
            }
        if (!selected)
            return VNCDESKTOP_NO_DESKTOP;

        desktop_backend *backend = calloc(1, sizeof(*backend));
        if (!backend)
            return VNCDESKTOP_NO_MEMORY;
        VNCDesktopCaptureState *state = [VNCDesktopCaptureState new];
        state.filter = [[SCContentFilter alloc] initWithDisplay:selected
                                              excludingWindows:@[]];
        state.configuration = [SCStreamConfiguration new];
        /* The VNC server supplies its own pointer cursor. */
        state.configuration.showsCursor = NO;
        state.configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        state.configuration.scalesToFit = YES;
        backend->state = (void *)CFBridgingRetain(state);
        backend->display = display;
        backend->source = CGEventSourceCreate(kCGEventSourceStatePrivate);
        if (!backend->source) {
            desktop_close(backend);
            return VNCDESKTOP_NO_MEMORY;
        }
        backend->pointer = CGDisplayBounds(display).origin;
        *result = backend;
        return VNCDESKTOP_OK;
    }
}

int desktop_capture(desktop_backend *backend, desktop_frame *frame)
{
    @autoreleasepool {
        if (!CGDisplayIsActive(backend->display))
            return VNCDESKTOP_NO_DESKTOP;
        if (!CGPreflightScreenCaptureAccess() || !AXIsProcessTrusted())
            return VNCDESKTOP_PERMISSION_DENIED;
        size_t width = CGDisplayPixelsWide(backend->display);
        size_t height = CGDisplayPixelsHigh(backend->display);
        if (!width || !height || width > 65535 || height > 65535 ||
            width > SIZE_MAX / 4 / height)
            return VNCDESKTOP_CAPTURE_FAILED;

        VNCDesktopCaptureState *state = (__bridge VNCDesktopCaptureState *)backend->state;
        state.configuration.width = width;
        state.configuration.height = height;
        VNCDesktopCaptureResult *request = [VNCDesktopCaptureResult new];
        [SCScreenshotManager captureImageWithFilter:state.filter
            configuration:state.configuration
            completionHandler:^(CGImageRef image, NSError *error) {
                request->image = image ? CGImageRetain(image) : NULL;
                request->error = error;
                dispatch_semaphore_signal(request->ready);
            }];
        dispatch_semaphore_wait(request->ready, DISPATCH_TIME_FOREVER);
        if (request->error || !request->image)
            return capture_error();

        if (backend->width != (int)width || backend->height != (int)height) {
            unsigned char *pixels = realloc(backend->pixels, width * height * 4);
            if (!pixels)
                return VNCDESKTOP_NO_MEMORY;
            backend->pixels = pixels;
            backend->width = (int)width;
            backend->height = (int)height;
        }
        CGColorSpaceRef colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        if (!colorspace)
            return VNCDESKTOP_NO_MEMORY;
        CGContextRef context = CGBitmapContextCreate(backend->pixels,
            width, height, 8, width * 4, colorspace,
            kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGColorSpaceRelease(colorspace);
        if (!context)
            return VNCDESKTOP_NO_MEMORY;
        CGContextDrawImage(context, CGRectMake(0, 0, width, height), request->image);
        CGContextRelease(context);
        frame->width = backend->width;
        frame->height = backend->height;
        frame->pixels = backend->pixels;
        return VNCDESKTOP_OK;
    }
}

static void post_mouse(desktop_backend *backend, CGEventType type,
                       CGMouseButton button)
{
    if (type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown ||
        type == kCGEventOtherMouseDown) {
        CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        double dx = backend->pointer.x - backend->last_click[button].x;
        double dy = backend->pointer.y - backend->last_click[button].y;
        if (now - backend->last_click_time[button] < 0.5 && dx * dx + dy * dy < 25)
            backend->click_count[button] = backend->click_count[button] % 3 + 1;
        else
            backend->click_count[button] = 1;
        backend->last_click_time[button] = now;
        backend->last_click[button] = backend->pointer;
    }
    CGEventRef event = CGEventCreateMouseEvent(backend->source, type,
                                             backend->pointer, button);
    if (event) {
        if (type != kCGEventMouseMoved && type != kCGEventLeftMouseDragged &&
            type != kCGEventRightMouseDragged && type != kCGEventOtherMouseDragged)
            CGEventSetIntegerValueField(event, kCGMouseEventClickState,
                                       backend->click_count[button]);
        CGEventSetFlags(event, backend->flags);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
}

void desktop_pointer(desktop_backend *backend, int mask, int x, int y)
{
    if (backend->width <= 0 || backend->height <= 0)
        return;
    x = x < 0 ? 0 : (x >= backend->width ? backend->width - 1 : x);
    y = y < 0 ? 0 : (y >= backend->height ? backend->height - 1 : y);
    CGRect bounds = CGDisplayBounds(backend->display);
    backend->pointer = CGPointMake(bounds.origin.x + x * bounds.size.width / backend->width,
                                  bounds.origin.y + y * bounds.size.height / backend->height);
    CGEventType move = kCGEventMouseMoved;
    CGMouseButton button = kCGMouseButtonLeft;
    if (backend->buttons & 1)
        move = kCGEventLeftMouseDragged;
    else if (backend->buttons & 4) {
        move = kCGEventRightMouseDragged;
        button = kCGMouseButtonRight;
    } else if (backend->buttons & 2) {
        move = kCGEventOtherMouseDragged;
        button = kCGMouseButtonCenter;
    }
    post_mouse(backend, move, button);
    static const int bits[] = { 1, 4, 2 };
    static const CGEventType downs[] = {
        kCGEventLeftMouseDown, kCGEventRightMouseDown, kCGEventOtherMouseDown };
    static const CGEventType ups[] = {
        kCGEventLeftMouseUp, kCGEventRightMouseUp, kCGEventOtherMouseUp };
    for (int i = 0; i < 3; ++i)
        if ((mask ^ backend->buttons) & bits[i])
            post_mouse(backend, (mask & bits[i]) ? downs[i] : ups[i], (CGMouseButton)i);
    int pressed = mask & ~backend->buttons;
    int vertical = ((pressed & 8) != 0) - ((pressed & 16) != 0);
    int horizontal = ((pressed & 32) != 0) - ((pressed & 64) != 0);
    if (vertical || horizontal) {
        CGEventRef event = CGEventCreateScrollWheelEvent(backend->source,
            kCGScrollEventUnitLine, 2, vertical, horizontal);
        if (event) {
            CGEventSetFlags(event, backend->flags);
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
        }
    }
    backend->buttons = mask;
}

static int keycode_for_keysym(uint32_t key)
{
    static const int letters[] = {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E,
        kVK_ANSI_F, kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J,
        kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O,
        kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T,
        kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z };
    static const int numbers[] = { kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2,
        kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7,
        kVK_ANSI_8, kVK_ANSI_9 };
    static const int function[] = { kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5,
        kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12,
        kVK_F13, kVK_F14, kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20 };
    static const int keypad[] = { kVK_ANSI_Keypad0, kVK_ANSI_Keypad1,
        kVK_ANSI_Keypad2, kVK_ANSI_Keypad3, kVK_ANSI_Keypad4,
        kVK_ANSI_Keypad5, kVK_ANSI_Keypad6, kVK_ANSI_Keypad7,
        kVK_ANSI_Keypad8, kVK_ANSI_Keypad9 };
    if (key >= 'a' && key <= 'z') return letters[key - 'a'];
    if (key >= 'A' && key <= 'Z') return letters[key - 'A'];
    if (key >= '0' && key <= '9') return numbers[key - '0'];
    if (key >= XK_F1 && key <= XK_F20) return function[key - XK_F1];
    if (key >= XK_KP_0 && key <= XK_KP_9) return keypad[key - XK_KP_0];
    switch (key) {
    case '!': return kVK_ANSI_1;
    case '@': return kVK_ANSI_2;
    case '#': return kVK_ANSI_3;
    case '$': return kVK_ANSI_4;
    case '%': return kVK_ANSI_5;
    case '^': return kVK_ANSI_6;
    case '&': return kVK_ANSI_7;
    case '*': return kVK_ANSI_8;
    case '(': return kVK_ANSI_9;
    case ')': return kVK_ANSI_0;
    case '-': case '_': return kVK_ANSI_Minus;
    case '=': case '+': return kVK_ANSI_Equal;
    case '[': case '{': return kVK_ANSI_LeftBracket;
    case ']': case '}': return kVK_ANSI_RightBracket;
    case ';': case ':': return kVK_ANSI_Semicolon;
    case '\'': case '"': return kVK_ANSI_Quote;
    case '\\': case '|': return kVK_ANSI_Backslash;
    case ',': case '<': return kVK_ANSI_Comma;
    case '.': case '>': return kVK_ANSI_Period;
    case '/': case '?': return kVK_ANSI_Slash;
    case '`': case '~': return kVK_ANSI_Grave;
    case ' ': case XK_KP_Space: return kVK_Space;
    case XK_Return: return kVK_Return;
    case XK_Tab: case XK_KP_Tab: case 0xfe20: return kVK_Tab;
    case XK_BackSpace: return kVK_Delete;
    case XK_Delete: case XK_KP_Delete: return kVK_ForwardDelete;
    case XK_Escape: return kVK_Escape;
    case XK_Home: case XK_KP_Home: return kVK_Home;
    case XK_End: case XK_KP_End: return kVK_End;
    case XK_Page_Up: case XK_KP_Page_Up: return kVK_PageUp;
    case XK_Page_Down: case XK_KP_Page_Down: return kVK_PageDown;
    case XK_Left: case XK_KP_Left: return kVK_LeftArrow;
    case XK_Right: case XK_KP_Right: return kVK_RightArrow;
    case XK_Up: case XK_KP_Up: return kVK_UpArrow;
    case XK_Down: case XK_KP_Down: return kVK_DownArrow;
    case XK_Shift_L: return kVK_Shift;
    case XK_Shift_R: return kVK_RightShift;
    case XK_Control_L: return kVK_Control;
    case XK_Control_R: return kVK_RightControl;
    case XK_Alt_L: return kVK_Option;
    case XK_Alt_R: return kVK_RightOption;
    case XK_Meta_L: case XK_Super_L: return kVK_Command;
    case XK_Meta_R: case XK_Super_R: return kVK_RightCommand;
    case XK_Caps_Lock: return kVK_CapsLock;
    case XK_KP_Enter: return kVK_ANSI_KeypadEnter;
    case XK_KP_Equal: return kVK_ANSI_KeypadEquals;
    case XK_KP_Multiply: return kVK_ANSI_KeypadMultiply;
    case XK_KP_Add: return kVK_ANSI_KeypadPlus;
    case XK_KP_Subtract: return kVK_ANSI_KeypadMinus;
    case XK_KP_Decimal: return kVK_ANSI_KeypadDecimal;
    case XK_KP_Divide: return kVK_ANSI_KeypadDivide;
    case XK_Clear: case XK_Num_Lock: return kVK_ANSI_KeypadClear;
    case XK_Help: case XK_Insert: return kVK_Help;
    default: return -1;
    }
}

static void update_flags(desktop_backend *backend)
{
    backend->flags = 0;
    if (backend->keys[kVK_Shift] || backend->keys[kVK_RightShift])
        backend->flags |= kCGEventFlagMaskShift;
    if (backend->keys[kVK_Control] || backend->keys[kVK_RightControl])
        backend->flags |= kCGEventFlagMaskControl;
    if (backend->keys[kVK_Option] || backend->keys[kVK_RightOption])
        backend->flags |= kCGEventFlagMaskAlternate;
    if (backend->keys[kVK_Command] || backend->keys[kVK_RightCommand])
        backend->flags |= kCGEventFlagMaskCommand;
}

void desktop_key(desktop_backend *backend, int down, uint32_t keysym)
{
    int code = keycode_for_keysym(keysym);
    int repeat = code >= 0 && down && backend->keys[code];
    uint32_t unicode = 0;
    if ((keysym >= 0x20 && keysym <= 0x7e) ||
        (keysym >= 0xa0 && keysym <= 0xff))
        unicode = keysym;
    else if ((keysym & 0xff000000) == 0x01000000)
        unicode = keysym & 0xffffff;
    if (unicode > 0x10ffff || (unicode >= 0xd800 && unicode <= 0xdfff))
        unicode = 0;
    if (code < 0 && !unicode)
        return;
    /* Characters without a physical ANSI key are injected as complete text
     * keystrokes, so a disconnect cannot strand their synthetic key code. */
    if (code < 0 && (!down || (backend->flags & (kCGEventFlagMaskControl |
                       kCGEventFlagMaskAlternate | kCGEventFlagMaskCommand))))
        return;
    if (code >= 0) {
        backend->keys[code] = down != 0;
        update_flags(backend);
    }
    CGEventRef event = CGEventCreateKeyboardEvent(backend->source,
        code >= 0 ? (CGKeyCode)code : (CGKeyCode)0, down != 0);
    if (!event)
        return;
    CGEventSetFlags(event, backend->flags);
    CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, repeat);
    /* Unicode preserves typed text across keyboard layouts. Shortcuts keep
     * physical key codes (the standard ANSI layout) with their modifiers. */
    if (unicode && !(backend->flags & (kCGEventFlagMaskControl |
                                     kCGEventFlagMaskAlternate |
                                     kCGEventFlagMaskCommand))) {
        UniChar chars[2];
        if (unicode <= 0xffff) {
            chars[0] = (UniChar)unicode;
            CGEventKeyboardSetUnicodeString(event, 1, chars);
        } else {
            unicode -= 0x10000;
            chars[0] = (UniChar)(0xd800 + (unicode >> 10));
            chars[1] = (UniChar)(0xdc00 + (unicode & 0x3ff));
            CGEventKeyboardSetUnicodeString(event, 2, chars);
        }
    }
    CGEventPost(kCGHIDEventTap, event);
    if (code < 0) {
        CGEventSetType(event, kCGEventKeyUp);
        CGEventPost(kCGHIDEventTap, event);
    }
    CFRelease(event);
}

void desktop_release_inputs(desktop_backend *backend)
{
    backend->flags = 0;
    for (int code = 0; code < 128; ++code) {
        if (!backend->keys[code])
            continue;
        CGEventRef event = CGEventCreateKeyboardEvent(backend->source, (CGKeyCode)code, false);
        if (event) {
            CGEventSetFlags(event, 0);
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
        }
        backend->keys[code] = 0;
    }
    if (backend->buttons & 1)
        post_mouse(backend, kCGEventLeftMouseUp, kCGMouseButtonLeft);
    if (backend->buttons & 4)
        post_mouse(backend, kCGEventRightMouseUp, kCGMouseButtonRight);
    if (backend->buttons & 2)
        post_mouse(backend, kCGEventOtherMouseUp, kCGMouseButtonCenter);
    backend->buttons = 0;
}

void desktop_close(desktop_backend *backend)
{
    if (!backend)
        return;
    desktop_release_inputs(backend);
    if (backend->source)
        CFRelease(backend->source);
    @autoreleasepool {
        if (backend->state) {
            VNCDesktopCaptureState *state = CFBridgingRelease(backend->state);
            (void)state;
        }
    }
    free(backend->pixels);
    free(backend);
}
