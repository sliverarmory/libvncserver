/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Run in a disposable interactive Windows session. Input is sent only after
 * checking that this test's own window owns focus and covers the target point. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rfb/keysym.h>
#include "../src/desktop/backend.h"

static struct {
    unsigned int button_down, button_up, key_down, key_up, characters;
    int mouse_x, mouse_y;
    WCHAR character;
    COLORREF color;
} observed;

static LRESULT CALLBACK test_window_proc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        RECT rectangle;
        HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(observed.color);
        GetClientRect(window, &rectangle);
        FillRect(dc, &rectangle, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        ++observed.button_down;
        observed.mouse_x = (short)LOWORD(lparam);
        observed.mouse_y = (short)HIWORD(lparam);
        return 0;
    case WM_LBUTTONUP:
        ++observed.button_up;
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_F6) ++observed.key_down;
        return 0;
    case WM_KEYUP:
        if (wparam == VK_F6) ++observed.key_up;
        return 0;
    case WM_CHAR:
        ++observed.characters;
        observed.character = (WCHAR)wparam;
        return 0;
    case WM_CLOSE:
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

static void pump_messages(void)
{
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static int focus_window(HWND window)
{
    int attempt;
    for (attempt = 0; attempt < 100; ++attempt) {
        SetForegroundWindow(window);
        SetFocus(window);
        pump_messages();
        if (GetForegroundWindow() == window && GetFocus() == window)
            return 1;
        Sleep(20);
    }
    return 0;
}

static int owns_input(HWND window, POINT point)
{
    pump_messages();
    return GetForegroundWindow() == window && GetFocus() == window &&
           WindowFromPoint(point) == window;
}

static int wait_counter(HWND window, const unsigned int *counter,
                         unsigned int expected)
{
    int attempt;
    for (attempt = 0; attempt < 200; ++attempt) {
        pump_messages();
        if (GetForegroundWindow() != window || GetFocus() != window)
            return 0;
        if (*counter >= expected)
            return 1;
        Sleep(10);
    }
    return 0;
}

static int wait_pixel(desktop_backend *backend, HWND window, POINT point,
                       COLORREF expected)
{
    int attempt;
    for (attempt = 0; attempt < 100; ++attempt) {
        desktop_frame frame;
        const unsigned char *pixel;
        int x = point.x - GetSystemMetrics(SM_XVIRTUALSCREEN);
        int y = point.y - GetSystemMetrics(SM_YVIRTUALSCREEN);
        pump_messages();
        if (WindowFromPoint(point) != window ||
            desktop_capture(backend, &frame) != VNCDESKTOP_OK)
            return 0;
        if (x < 0 || y < 0 || x >= frame.width || y >= frame.height)
            return 0;
        pixel = frame.pixels + ((size_t)y * frame.width + x) * 4;
        if (pixel[0] == GetBValue(expected) && pixel[1] == GetGValue(expected) &&
            pixel[2] == GetRValue(expected))
            return 1;
        Sleep(20);
    }
    return 0;
}

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s (Win32 error %lu)\n", message, GetLastError()); \
        goto cleanup; \
    } \
} while (0)

int main(void)
{
    const WCHAR class_name[] = L"VNCDesktopNativeTest";
    desktop_backend *backend = NULL;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class;
    HWND window = NULL;
    POINT point = {80, 80};
    int x, y, result = 1;
    ATOM registered = 0;

    /* Opening first scopes DPI awareness before the test creates any windows. */
    REQUIRE(desktop_open(&backend) == VNCDESKTOP_OK && backend,
            "Native Windows test requires a visible interactive desktop");
    REQUIRE(GetSystemMetrics(SM_CXSCREEN) >= 480 && GetSystemMetrics(SM_CYSCREEN) >= 360,
            "Windows test display must be at least 480x360");
    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = test_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    registered = RegisterClassW(&window_class);
    REQUIRE(registered, "Cannot register native test window");
    observed.color = RGB(0x12, 0x34, 0x56);
    window = CreateWindowExW(WS_EX_TOPMOST, class_name, L"VNC desktop backend test",
        WS_OVERLAPPEDWINDOW, 60, 60, 360, 260, NULL, NULL, instance, NULL);
    REQUIRE(window, "Cannot create native test window");
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    REQUIRE(focus_window(window),
            "Cannot focus owned test window; refusing to inject input elsewhere");
    REQUIRE(ClientToScreen(window, &point), "Cannot locate test window pixels");
    REQUIRE(owns_input(window, point), "Owned test window is not visible and focused");
    REQUIRE(wait_pixel(backend, window, point, observed.color),
            "Capture did not contain the owned window's expected BGR pixel");
    SetCapture(window);
    REQUIRE(GetCapture() == window, "Cannot capture mouse input to owned window");
    x = point.x - GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = point.y - GetSystemMetrics(SM_YVIRTUALSCREEN);

    REQUIRE(owns_input(window, point), "Focus lost before pointer injection");
    desktop_pointer(backend, 1, x, y);
    REQUIRE(wait_counter(window, &observed.button_down, 1),
            "Pointer press was not delivered to the owned window");
    /* Absolute mouse coordinates can round by one pixel on large desktops. */
    REQUIRE(abs(observed.mouse_x - 80) <= 1 && abs(observed.mouse_y - 80) <= 1,
            "Pointer coordinates were not mapped to the owned window correctly");
    REQUIRE(owns_input(window, point), "Focus lost before pointer release");
    desktop_pointer(backend, 0, x, y);
    REQUIRE(wait_counter(window, &observed.button_up, 1),
            "Pointer release was not delivered to the owned window");

    REQUIRE(owns_input(window, point), "Focus lost before keyboard injection");
    desktop_key(backend, 1, XK_F6);
    REQUIRE(wait_counter(window, &observed.key_down, 1),
            "Function key press was not delivered to the owned window");
    REQUIRE(owns_input(window, point), "Focus lost before keyboard release");
    desktop_key(backend, 0, XK_F6);
    REQUIRE(wait_counter(window, &observed.key_up, 1),
            "Function key release was not delivered to the owned window");

    REQUIRE(owns_input(window, point), "Focus lost before text injection");
    desktop_key(backend, 1, 'a');
    desktop_key(backend, 0, 'a');
    REQUIRE(wait_counter(window, &observed.characters, 1) && observed.character == L'a',
            "Unicode text was not delivered to the owned window");
    REQUIRE(owns_input(window, point), "Focus lost before uppercase text injection");
    desktop_key(backend, 1, 'A');
    desktop_key(backend, 0, 'A');
    REQUIRE(wait_counter(window, &observed.characters, 2) && observed.character == L'A',
            "Uppercase text was not delivered without an explicit Shift event");

    REQUIRE(owns_input(window, point), "Focus lost before held-input test");
    desktop_pointer(backend, 1, x, y);
    desktop_key(backend, 1, XK_F6);
    REQUIRE(wait_counter(window, &observed.button_down, 2) &&
            wait_counter(window, &observed.key_down, 2),
            "Held key/button were not delivered to the owned window");
    REQUIRE(owns_input(window, point), "Focus lost before held-input cleanup");
    desktop_release_inputs(backend);
    REQUIRE(wait_counter(window, &observed.button_up, 2) &&
            wait_counter(window, &observed.key_up, 2),
            "Disconnect cleanup failed to release held key/button");

    observed.color = RGB(0x89, 0xab, 0xcd);
    InvalidateRect(window, NULL, FALSE);
    UpdateWindow(window);
    REQUIRE(wait_pixel(backend, window, point, observed.color),
            "Capture did not update after repainting the owned window");
    puts("Windows native capture, live updates, pointer, keyboard, text, and input cleanup passed");
    result = 0;

cleanup:
    /* Release before destroying the window so any outstanding ups still have
     * their original target. No further down events are sent during cleanup. */
    desktop_close(backend);
    pump_messages();
    if (GetCapture() == window) ReleaseCapture();
    if (window) DestroyWindow(window);
    if (registered) UnregisterClassW(class_name, instance);
    return result;
}
