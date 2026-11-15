/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Run on a disposable Xvfb display: this test creates/focuses a window and
 * injects keyboard and pointer input into that display. */
#define _POSIX_C_SOURCE 200809L
#include "../src/desktop/backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <xcb/xcb.h>

static void pause_briefly(void)
{
    struct timespec delay = { 0, 10000000 };
    nanosleep(&delay, NULL);
}

static int check_request(xcb_connection_t *connection, xcb_void_cookie_t cookie)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error) {
        fprintf(stderr, "X11 request failed: error %u\n", error->error_code);
        free(error);
        return 0;
    }
    return 1;
}

static int wait_pointer(xcb_connection_t *connection, xcb_window_t root,
                        int x, int y, int button)
{
    int attempt;
    for (attempt = 0; attempt < 200; ++attempt) {
        xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(connection,
            xcb_query_pointer(connection, root), NULL);
        int matches = reply && reply->root_x == x && reply->root_y == y &&
            !!(reply->mask & XCB_BUTTON_MASK_1) == button;
        free(reply);
        if (matches)
            return 1;
        pause_briefly();
    }
    return 0;
}

static int wait_key(xcb_connection_t *connection, xcb_window_t window,
                   uint8_t type, uint8_t keycode, int shifted,
                   uint8_t *received_keycode)
{
    int attempt;
    for (attempt = 0; attempt < 200; ++attempt) {
        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(connection)) != NULL) {
            if ((event->response_type & 0x7f) == type) {
                xcb_key_press_event_t *key = (xcb_key_press_event_t *)event;
                int matches = key->event == window &&
                    (!keycode || key->detail == keycode) &&
                    (shifted < 0 || !!(key->state & XCB_MOD_MASK_SHIFT) == shifted);
                if (matches) {
                    if (received_keycode)
                        *received_keycode = key->detail;
                    free(event);
                    return 1;
                }
            }
            free(event);
        }
        pause_briefly();
    }
    return 0;
}

static int check_pixel(desktop_backend *backend, int x, int y,
                       unsigned int red, unsigned int green, unsigned int blue)
{
    desktop_frame frame;
    const unsigned char *pixel;
    int status = desktop_capture(backend, &frame);
    if (status != VNCDESKTOP_OK) {
        fprintf(stderr, "Capture failed: %d\n", status);
        return 0;
    }
    if (frame.width <= x || frame.height <= y || !frame.pixels)
        return 0;
    pixel = frame.pixels + ((size_t)y * frame.width + x) * 4;
    if (pixel[0] != blue || pixel[1] != green || pixel[2] != red || pixel[3]) {
        fprintf(stderr, "Pixel mismatch: BGR=%u,%u,%u; expected %u,%u,%u\n",
                pixel[0], pixel[1], pixel[2], blue, green, red);
        return 0;
    }
    return 1;
}

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s\n", message); \
        goto cleanup; \
    } \
} while (0)

int main(void)
{
    desktop_backend *backend = NULL;
    xcb_connection_t *connection = NULL;
    xcb_screen_iterator_t screens;
    xcb_screen_t *screen;
    xcb_window_t window = XCB_NONE;
    xcb_alloc_color_reply_t *color = NULL;
    xcb_get_keyboard_mapping_reply_t *mapping = NULL;
    uint32_t attributes[2];
    uint8_t letter_key = 0;
    int screen_number, result = 1;

    connection = xcb_connect(NULL, &screen_number);
    REQUIRE(connection && !xcb_connection_has_error(connection),
            "This test requires a running disposable Xvfb display");
    screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (screen_number-- > 0 && screens.rem)
        xcb_screen_next(&screens);
    REQUIRE(screens.rem, "No X11 screen");
    screen = screens.data;
    REQUIRE(screen->width_in_pixels >= 200 && screen->height_in_pixels >= 150,
            "Xvfb display must be at least 200x150");
    REQUIRE(screen->root_depth == 24, "Use a 24-bit Xvfb screen for this test");

    color = xcb_alloc_color_reply(connection,
        xcb_alloc_color(connection, screen->default_colormap,
                        0x1212, 0x3434, 0x5656), NULL);
    REQUIRE(color, "Cannot allocate test color");
    attributes[0] = color->pixel;
    attributes[1] = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE;
    window = xcb_generate_id(connection);
    REQUIRE(check_request(connection, xcb_create_window_checked(connection,
        screen->root_depth, window, screen->root, 20, 30, 160, 100, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, attributes)),
        "Cannot create test window");
    REQUIRE(check_request(connection, xcb_map_window_checked(connection, window)),
            "Cannot map test window");
    REQUIRE(check_request(connection, xcb_set_input_focus_checked(connection,
        XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME)),
        "Cannot focus test window");

    REQUIRE(desktop_open(&backend) == VNCDESKTOP_OK && backend,
            "Cannot open X11 desktop backend");
    REQUIRE(check_pixel(backend, 25, 35, 0x12, 0x34, 0x56),
            "Capture did not contain the real X11 window");

    desktop_pointer(backend, 1, 40, 50);
    REQUIRE(wait_pointer(connection, screen->root, 40, 50, 1),
            "Pointer move/button press was not delivered");
    desktop_pointer(backend, 0, 41, 51);
    REQUIRE(wait_pointer(connection, screen->root, 41, 51, 0),
            "Pointer move/button release was not delivered");

    desktop_key(backend, 1, 'a');
    REQUIRE(wait_key(connection, window, XCB_KEY_PRESS, 0, 0, &letter_key),
            "Keyboard press was not delivered to the focused window");
    mapping = xcb_get_keyboard_mapping_reply(connection,
        xcb_get_keyboard_mapping(connection, letter_key, 1), NULL);
    REQUIRE(mapping && mapping->keysyms_per_keycode &&
            xcb_get_keyboard_mapping_keysyms(mapping)[0] == 'a',
            "Keyboard press used the wrong physical keycode");
    desktop_key(backend, 0, 'a');
    REQUIRE(wait_key(connection, window, XCB_KEY_RELEASE, letter_key, -1, NULL),
            "Keyboard release was not delivered");

    /* A viewer may send uppercase keysyms without explicit Shift events. */
    desktop_key(backend, 1, 'A');
    REQUIRE(wait_key(connection, window, XCB_KEY_PRESS, letter_key, 1, NULL),
            "Uppercase key did not carry the Shift modifier");
    desktop_key(backend, 0, 'A');
    REQUIRE(wait_key(connection, window, XCB_KEY_RELEASE, letter_key, -1, NULL),
            "Uppercase key release was not delivered");

    desktop_pointer(backend, 1, 42, 52);
    REQUIRE(wait_pointer(connection, screen->root, 42, 52, 1),
            "Held pointer button was not delivered");
    desktop_key(backend, 1, 'a');
    REQUIRE(wait_key(connection, window, XCB_KEY_PRESS, letter_key, 0, NULL),
            "Held keyboard key was not delivered");
    desktop_release_inputs(backend);
    REQUIRE(wait_key(connection, window, XCB_KEY_RELEASE, letter_key, -1, NULL),
            "Disconnect cleanup did not release the held key");
    REQUIRE(wait_pointer(connection, screen->root, 42, 52, 0),
            "Disconnect cleanup did not release the held button");

    attributes[0] = screen->white_pixel;
    REQUIRE(check_request(connection, xcb_change_window_attributes_checked(
        connection, window, XCB_CW_BACK_PIXEL, attributes)),
        "Cannot change test window color");
    REQUIRE(check_request(connection, xcb_clear_area_checked(
        connection, 0, window, 0, 0, 160, 100)),
        "Cannot repaint test window");
    REQUIRE(check_pixel(backend, 25, 35, 255, 255, 255),
            "Capture did not update after the desktop changed");
    puts("X11 desktop capture, live updates, pointer, keyboard and held-input cleanup passed");
    result = 0;

cleanup:
    desktop_close(backend);
    free(mapping);
    free(color);
    if (connection) {
        if (window)
            xcb_destroy_window(connection, window);
        xcb_flush(connection);
        xcb_disconnect(connection);
    }
    return result;
}
