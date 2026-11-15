/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <rfb/keysym.h>
#include "backend.h"

/* This backend runs inside the caller's interactive session. It does not switch
 * desktops, alter process DPI policy, or inject into higher-integrity windows. */
typedef HANDLE (WINAPI *set_thread_dpi_fn)(HANDLE);

typedef struct held_key {
    uint32_t keysym;
    WORD vk;
    WORD unicode[2];
    unsigned int units;
    DWORD flags;
} held_key;

struct desktop_backend {
    HDC memory_dc;
    HBITMAP bitmap;
    HGDIOBJ original_bitmap;
    unsigned char *pixels;
    int width, height, left, top;
    int buttons;
    WCHAR desktop_name[256];
    set_thread_dpi_fn set_thread_dpi;
    HANDLE old_dpi;
    held_key keys[128];
};

static int on_input_desktop(desktop_backend *backend)
{
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    WCHAR name[256];
    DWORD needed;
    BOOL ok;
    if (!desktop)
        return 0;
    ok = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed);
    CloseDesktop(desktop);
    return ok && wcscmp(name, backend->desktop_name) == 0;
}

static int resize_bitmap(desktop_backend *backend, HDC screen,
                         int width, int height)
{
    BITMAPINFO info;
    HBITMAP bitmap;
    HGDIOBJ previous;
    void *pixels = NULL;

    /* RFB dimensions are 16-bit; reject dimensions and sizes before GDI sees
     * them. A 32-bit DIB's rows are already aligned to DWORD boundaries. */
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535 ||
        (size_t)width > SIZE_MAX / 4 / (size_t)height)
        return VNCDESKTOP_CAPTURE_FAILED;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!bitmap)
        return VNCDESKTOP_NO_MEMORY;
    previous = SelectObject(backend->memory_dc, bitmap);
    if (!previous || previous == HGDI_ERROR) {
        DeleteObject(bitmap);
        return VNCDESKTOP_CAPTURE_FAILED;
    }
    if (backend->bitmap)
        DeleteObject(backend->bitmap);
    else
        backend->original_bitmap = previous;
    backend->bitmap = bitmap;
    backend->pixels = pixels;
    backend->width = width;
    backend->height = height;
    return VNCDESKTOP_OK;
}

int desktop_open(desktop_backend **result)
{
    desktop_backend *backend;
    HDESK desktop;
    USEROBJECTFLAGS station_flags;
    DWORD needed;
    HMODULE user32;
    desktop_frame frame;
    int status;

    *result = NULL;
    backend = calloc(1, sizeof(*backend));
    if (!backend)
        return VNCDESKTOP_NO_MEMORY;
    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS,
            &station_flags, sizeof(station_flags), &needed) ||
        !(station_flags.dwFlags & WSF_VISIBLE)) {
        desktop_close(backend);
        return VNCDESKTOP_NO_DESKTOP;
    }
    desktop = GetThreadDesktop(GetCurrentThreadId());
    if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME,
            backend->desktop_name, sizeof(backend->desktop_name), &needed) ||
        !on_input_desktop(backend)) {
        desktop_close(backend);
        return VNCDESKTOP_NO_DESKTOP;
    }

    user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        FARPROC proc = GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        /* Avoid incompatible-function-pointer casts with MinGW diagnostics. */
        memcpy(&backend->set_thread_dpi, &proc, sizeof(proc));
    }
    if (backend->set_thread_dpi) {
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, with a V1 fallback. */
        backend->old_dpi = backend->set_thread_dpi((HANDLE)(INT_PTR)-4);
        if (!backend->old_dpi)
            backend->old_dpi = backend->set_thread_dpi((HANDLE)(INT_PTR)-3);
    }
    backend->memory_dc = CreateCompatibleDC(NULL);
    if (!backend->memory_dc) {
        desktop_close(backend);
        return VNCDESKTOP_CAPTURE_FAILED;
    }
    status = desktop_capture(backend, &frame);
    if (status != VNCDESKTOP_OK) {
        desktop_close(backend);
        return status;
    }
    *result = backend;
    return VNCDESKTOP_OK;
}

int desktop_capture(desktop_backend *backend, desktop_frame *frame)
{
    int width, height, status;
    HDC screen;
    BOOL copied;
    if (!on_input_desktop(backend))
        return VNCDESKTOP_NO_DESKTOP;
    width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    backend->left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    backend->top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    screen = GetDC(NULL);
    if (!screen)
        return VNCDESKTOP_CAPTURE_FAILED;
    if (!backend->bitmap || width != backend->width || height != backend->height) {
        status = resize_bitmap(backend, screen, width, height);
        if (status != VNCDESKTOP_OK) {
            ReleaseDC(NULL, screen);
            return status;
        }
    }
    copied = BitBlt(backend->memory_dc, 0, 0, width, height, screen,
                    backend->left, backend->top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, screen);
    /* GDI batches calls; synchronize before the worker reads DIB memory. */
    if (!GdiFlush() || !copied)
        return VNCDESKTOP_CAPTURE_FAILED;
    frame->width = width;
    frame->height = height;
    frame->pixels = backend->pixels;
    return VNCDESKTOP_OK;
}

static void send_mouse_event(DWORD flags, DWORD data)
{
    INPUT event;
    memset(&event, 0, sizeof(event));
    event.type = INPUT_MOUSE;
    event.mi.dwFlags = flags;
    event.mi.mouseData = data;
    SendInput(1, &event, sizeof(event));
}

void desktop_pointer(desktop_backend *backend, int mask, int x, int y)
{
    static const DWORD down_flags[] = {
        MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_RIGHTDOWN
    };
    static const DWORD up_flags[] = {
        MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP
    };
    INPUT event;
    int button;
    if (!on_input_desktop(backend))
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= backend->width) x = backend->width - 1;
    if (y >= backend->height) y = backend->height - 1;
    memset(&event, 0, sizeof(event));
    event.type = INPUT_MOUSE;
    event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    event.mi.dx = backend->width > 1 ? (LONG)((int64_t)x * 65535 / (backend->width - 1)) : 0;
    event.mi.dy = backend->height > 1 ? (LONG)((int64_t)y * 65535 / (backend->height - 1)) : 0;
    SendInput(1, &event, sizeof(event));
    for (button = 0; button < 3; ++button) {
        int bit = 1 << button;
        if ((mask ^ backend->buttons) & bit)
            send_mouse_event(mask & bit ? down_flags[button] : up_flags[button], 0);
    }
    if ((mask & 8) && !(backend->buttons & 8))
        send_mouse_event(MOUSEEVENTF_WHEEL, WHEEL_DELTA);
    if ((mask & 16) && !(backend->buttons & 16))
        send_mouse_event(MOUSEEVENTF_WHEEL, (DWORD)-WHEEL_DELTA);
    if ((mask & 32) && !(backend->buttons & 32))
        send_mouse_event(MOUSEEVENTF_HWHEEL, (DWORD)-WHEEL_DELTA);
    if ((mask & 64) && !(backend->buttons & 64))
        send_mouse_event(MOUSEEVENTF_HWHEEL, WHEEL_DELTA);
    backend->buttons = mask;
}

static WORD special_key(uint32_t keysym, DWORD *flags)
{
    if (keysym >= XK_F1 && keysym <= XK_F24)
        return (WORD)(VK_F1 + keysym - XK_F1);
    if (keysym >= XK_KP_0 && keysym <= XK_KP_9)
        return (WORD)(VK_NUMPAD0 + keysym - XK_KP_0);
    switch (keysym) {
    case XK_BackSpace: return VK_BACK;
    case XK_Tab: case XK_ISO_Left_Tab: case XK_KP_Tab: return VK_TAB;
    case XK_Return: return VK_RETURN;
    case XK_KP_Enter: *flags = KEYEVENTF_EXTENDEDKEY; return VK_RETURN;
    case XK_Escape: return VK_ESCAPE;
    case XK_Pause: return VK_PAUSE;
    case XK_Caps_Lock: return VK_CAPITAL;
    case XK_Num_Lock: *flags = KEYEVENTF_EXTENDEDKEY; return VK_NUMLOCK;
    case XK_Scroll_Lock: return VK_SCROLL;
    case XK_Shift_L: return VK_LSHIFT;
    case XK_Shift_R: return VK_RSHIFT;
    case XK_Control_L: return VK_LCONTROL;
    case XK_Control_R: *flags = KEYEVENTF_EXTENDEDKEY; return VK_RCONTROL;
    case XK_Alt_L: case XK_Meta_L: return VK_LMENU;
    case XK_Alt_R: case XK_Meta_R: *flags = KEYEVENTF_EXTENDEDKEY; return VK_RMENU;
    case XK_Super_L: *flags = KEYEVENTF_EXTENDEDKEY; return VK_LWIN;
    case XK_Super_R: *flags = KEYEVENTF_EXTENDEDKEY; return VK_RWIN;
    case XK_Menu: *flags = KEYEVENTF_EXTENDEDKEY; return VK_APPS;
    case XK_Print: *flags = KEYEVENTF_EXTENDEDKEY; return VK_SNAPSHOT;
    case XK_KP_Add: return VK_ADD;
    case XK_KP_Subtract: return VK_SUBTRACT;
    case XK_KP_Multiply: return VK_MULTIPLY;
    case XK_KP_Divide: *flags = KEYEVENTF_EXTENDEDKEY; return VK_DIVIDE;
    case XK_KP_Decimal: return VK_DECIMAL;
    case XK_KP_Separator: return VK_SEPARATOR;
    case XK_KP_Space: return VK_SPACE;
    case XK_Insert: case XK_KP_Insert: *flags = KEYEVENTF_EXTENDEDKEY; return VK_INSERT;
    case XK_Delete: case XK_KP_Delete: *flags = KEYEVENTF_EXTENDEDKEY; return VK_DELETE;
    case XK_Home: case XK_KP_Home: *flags = KEYEVENTF_EXTENDEDKEY; return VK_HOME;
    case XK_End: case XK_KP_End: *flags = KEYEVENTF_EXTENDEDKEY; return VK_END;
    case XK_Left: case XK_KP_Left: *flags = KEYEVENTF_EXTENDEDKEY; return VK_LEFT;
    case XK_Right: case XK_KP_Right: *flags = KEYEVENTF_EXTENDEDKEY; return VK_RIGHT;
    case XK_Up: case XK_KP_Up: *flags = KEYEVENTF_EXTENDEDKEY; return VK_UP;
    case XK_Down: case XK_KP_Down: *flags = KEYEVENTF_EXTENDEDKEY; return VK_DOWN;
    case XK_Page_Up: case XK_KP_Page_Up: *flags = KEYEVENTF_EXTENDEDKEY; return VK_PRIOR;
    case XK_Page_Down: case XK_KP_Page_Down: *flags = KEYEVENTF_EXTENDEDKEY; return VK_NEXT;
    default: return 0;
    }
}

static int shortcut_modifier_down(desktop_backend *backend)
{
    size_t i;
    for (i = 0; i < sizeof(backend->keys) / sizeof(backend->keys[0]); ++i) {
        WORD vk = backend->keys[i].vk;
        if (backend->keys[i].keysym &&
            (vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU ||
             vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN))
            return 1;
    }
    return 0;
}

static void send_key(const held_key *key, int down)
{
    INPUT events[2];
    unsigned int i, count = key->units ? key->units : 1;
    memset(events, 0, sizeof(events));
    for (i = 0; i < count; ++i) {
        events[i].type = INPUT_KEYBOARD;
        events[i].ki.wVk = key->vk;
        events[i].ki.wScan = key->units ? key->unicode[i] : 0;
        events[i].ki.dwFlags = key->flags | (down ? 0 : KEYEVENTF_KEYUP);
    }
    SendInput(count, events, sizeof(INPUT));
}

void desktop_key(desktop_backend *backend, int down, uint32_t keysym)
{
    held_key *key = NULL, *available = NULL;
    uint32_t character;
    size_t i;
    if (!keysym || !on_input_desktop(backend))
        return;
    for (i = 0; i < sizeof(backend->keys) / sizeof(backend->keys[0]); ++i) {
        if (backend->keys[i].keysym == keysym) {
            key = &backend->keys[i];
            break;
        }
        if (!backend->keys[i].keysym && !available)
            available = &backend->keys[i];
    }
    if (!down) {
        if (key) {
            send_key(key, 0);
            memset(key, 0, sizeof(*key));
        }
        return;
    }
    if (key) {
        send_key(key, 1);
        return;
    }
    if (!available)
        return;
    key = available;
    memset(key, 0, sizeof(*key));
    key->vk = special_key(keysym, &key->flags);
    if (!key->vk) {
        if ((keysym >= 0x20 && keysym <= 0x7e) || (keysym >= 0xa0 && keysym <= 0xff))
            character = keysym;
        else if ((keysym & 0xff000000U) == 0x01000000U)
            character = keysym & 0x00ffffffU;
        else
            return;
        if (character > 0x10ffff || (character >= 0xd800 && character <= 0xdfff))
            return;
        if (character <= 0xffff && shortcut_modifier_down(backend)) {
            HWND foreground = GetForegroundWindow();
            DWORD thread = foreground ? GetWindowThreadProcessId(foreground, NULL) : 0;
            SHORT mapping = VkKeyScanExW((WCHAR)character, GetKeyboardLayout(thread));
            if (mapping != -1)
                key->vk = LOBYTE(mapping);
        }
        if (!key->vk) {
            key->flags = KEYEVENTF_UNICODE;
            key->units = character > 0xffff ? 2 : 1;
            if (key->units == 2) {
                character -= 0x10000;
                key->unicode[0] = (WORD)(0xd800 + (character >> 10));
                key->unicode[1] = (WORD)(0xdc00 + (character & 0x3ff));
            } else {
                key->unicode[0] = (WORD)character;
            }
        }
    }
    key->keysym = keysym;
    send_key(key, 1);
}

void desktop_release_inputs(desktop_backend *backend)
{
    size_t i;
    int active = on_input_desktop(backend);
    for (i = 0; i < sizeof(backend->keys) / sizeof(backend->keys[0]); ++i) {
        if (active && backend->keys[i].keysym)
            send_key(&backend->keys[i], 0);
        memset(&backend->keys[i], 0, sizeof(backend->keys[i]));
    }
    if (active) {
        if (backend->buttons & 1) send_mouse_event(MOUSEEVENTF_LEFTUP, 0);
        if (backend->buttons & 2) send_mouse_event(MOUSEEVENTF_MIDDLEUP, 0);
        if (backend->buttons & 4) send_mouse_event(MOUSEEVENTF_RIGHTUP, 0);
    }
    backend->buttons = 0;
}

void desktop_close(desktop_backend *backend)
{
    if (!backend)
        return;
    desktop_release_inputs(backend);
    if (backend->original_bitmap)
        SelectObject(backend->memory_dc, backend->original_bitmap);
    if (backend->bitmap)
        DeleteObject(backend->bitmap);
    if (backend->memory_dc)
        DeleteDC(backend->memory_dc);
    if (backend->old_dpi)
        backend->set_thread_dpi(backend->old_dpi);
    free(backend);
}
