/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "backend.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

/* Keep an independent XCB connection: unlike Xlib's default I/O handler, a
 * disconnected X server must not terminate the application loading this DLL. */
struct desktop_backend {
    xcb_connection_t *connection;
    xcb_window_t root;
    xcb_get_keyboard_mapping_reply_t *keymap;
    uint8_t first_keycode;
    uint8_t last_keycode;
    uint8_t depth;
    uint8_t bytes_per_pixel;
    uint8_t scanline_pad;
    uint8_t image_byte_order;
    uint32_t red_mask, green_mask, blue_mask;
    unsigned char *pixels;
    size_t capacity;
    int width, height;
    unsigned int buttons;
    uint32_t held_keysym[256];
};

static int refresh_keymap(desktop_backend *backend)
{
    xcb_get_keyboard_mapping_reply_t *mapping;
    mapping = xcb_get_keyboard_mapping_reply(backend->connection,
        xcb_get_keyboard_mapping(backend->connection, backend->first_keycode,
            (uint8_t)(backend->last_keycode - backend->first_keycode + 1)),
        NULL);
    if (!mapping || !mapping->keysyms_per_keycode) {
        free(mapping);
        return 0;
    }
    free(backend->keymap);
    backend->keymap = mapping;
    return 1;
}

int desktop_open(desktop_backend **result)
{
    desktop_backend *backend;
    const xcb_setup_t *setup;
    const xcb_query_extension_reply_t *xtest;
    xcb_query_extension_reply_t *xwayland;
    xcb_screen_iterator_t screens;
    xcb_depth_iterator_t depths;
    xcb_format_iterator_t formats;
    xcb_screen_t *screen;
    int screen_number, found_visual = 0;
    const char *display = getenv("DISPLAY");
    const char *session = getenv("XDG_SESSION_TYPE");

    *result = NULL;
    if (!display || !*display || (session && strcmp(session, "wayland") == 0))
        return VNCDESKTOP_NO_DESKTOP;
    backend = calloc(1, sizeof(*backend));
    if (!backend)
        return VNCDESKTOP_NO_MEMORY;
    backend->connection = xcb_connect(display, &screen_number);
    if (!backend->connection || xcb_connection_has_error(backend->connection))
        goto unavailable;

    /* Xwayland cannot capture the compositor's complete desktop. */
    xwayland = xcb_query_extension_reply(backend->connection,
        xcb_query_extension(backend->connection, 8, "XWAYLAND"), NULL);
    if (!xwayland)
        goto unavailable;
    if (xwayland->present) {
        free(xwayland);
        goto unavailable;
    }
    free(xwayland);
    xtest = xcb_get_extension_data(backend->connection, &xcb_test_id);
    if (!xtest || !xtest->present)
        goto unavailable;

    setup = xcb_get_setup(backend->connection);
    if (!setup)
        goto unavailable;
    screens = xcb_setup_roots_iterator(setup);
    while (screen_number-- > 0 && screens.rem)
        xcb_screen_next(&screens);
    if (!screens.rem)
        goto unavailable;
    screen = screens.data;
    backend->root = screen->root;
    backend->depth = screen->root_depth;
    backend->image_byte_order = setup->image_byte_order;
    backend->first_keycode = setup->min_keycode;
    backend->last_keycode = setup->max_keycode;

    /* The root visual's masks, not its depth alone, determine pixel colors. */
    for (depths = xcb_screen_allowed_depths_iterator(screen); depths.rem;
         xcb_depth_next(&depths)) {
        xcb_visualtype_iterator_t visuals =
            xcb_depth_visuals_iterator(depths.data);
        for (; visuals.rem; xcb_visualtype_next(&visuals)) {
            xcb_visualtype_t *visual = visuals.data;
            if (visual->visual_id != screen->root_visual)
                continue;
            if (visual->_class != XCB_VISUAL_CLASS_TRUE_COLOR)
                goto unavailable;
            backend->red_mask = visual->red_mask;
            backend->green_mask = visual->green_mask;
            backend->blue_mask = visual->blue_mask;
            found_visual = 1;
        }
    }
    for (formats = xcb_setup_pixmap_formats_iterator(setup); formats.rem;
         xcb_format_next(&formats)) {
        if (formats.data->depth != backend->depth)
            continue;
        if (formats.data->bits_per_pixel != 16 &&
            formats.data->bits_per_pixel != 24 &&
            formats.data->bits_per_pixel != 32)
            goto unavailable;
        backend->bytes_per_pixel = formats.data->bits_per_pixel / 8;
        backend->scanline_pad = formats.data->scanline_pad;
        break;
    }
    if (!found_visual || !backend->bytes_per_pixel ||
        (backend->scanline_pad != 8 && backend->scanline_pad != 16 &&
         backend->scanline_pad != 32) ||
        !backend->red_mask || !backend->green_mask || !backend->blue_mask ||
        !refresh_keymap(backend))
        goto unavailable;

    *result = backend;
    return VNCDESKTOP_OK;

unavailable:
    desktop_close(backend);
    return VNCDESKTOP_NO_DESKTOP;
}

static unsigned char component(uint32_t pixel, uint32_t mask)
{
    unsigned int shift = 0;
    while (!(mask & 1)) {
        mask >>= 1;
        ++shift;
    }
    return (unsigned char)(((uint64_t)((pixel >> shift) & mask) * 255) / mask);
}

int desktop_capture(desktop_backend *backend, desktop_frame *frame)
{
    xcb_generic_event_t *event;
    xcb_get_geometry_reply_t *geometry;
    xcb_get_image_reply_t *image = NULL;
    xcb_generic_error_t *error = NULL;
    const unsigned char *source;
    size_t stride, required, y, x;
    int attempts;

    if (xcb_connection_has_error(backend->connection))
        return VNCDESKTOP_CAPTURE_FAILED;
    while ((event = xcb_poll_for_event(backend->connection)) != NULL) {
        if ((event->response_type & 0x7f) == XCB_MAPPING_NOTIFY)
            refresh_keymap(backend);
        free(event);
    }

    /* A display resize between GetGeometry and GetImage can produce BadMatch.
     * Re-query once, rather than permanently stopping at that normal race. */
    for (attempts = 0; attempts < 2; ++attempts) {
        geometry = xcb_get_geometry_reply(backend->connection,
            xcb_get_geometry(backend->connection, backend->root), NULL);
        if (!geometry)
            return VNCDESKTOP_CAPTURE_FAILED;
        backend->width = geometry->width;
        backend->height = geometry->height;
        free(geometry);
        if (backend->width <= 0 || backend->height <= 0 ||
            backend->width > INT16_MAX || backend->height > INT16_MAX)
            return VNCDESKTOP_CAPTURE_FAILED;
        image = xcb_get_image_reply(backend->connection,
            xcb_get_image(backend->connection, XCB_IMAGE_FORMAT_Z_PIXMAP,
                backend->root, 0, 0, (uint16_t)backend->width,
                (uint16_t)backend->height, UINT32_MAX), &error);
        if (image)
            break;
        if (!error || error->error_code != XCB_MATCH) {
            free(error);
            return VNCDESKTOP_CAPTURE_FAILED;
        }
        free(error);
        error = NULL;
    }
    free(error);
    if (!image)
        return VNCDESKTOP_CAPTURE_FAILED;

    stride = (((size_t)backend->width * backend->bytes_per_pixel * 8 +
               backend->scanline_pad - 1) / backend->scanline_pad) *
             (backend->scanline_pad / 8);
    if (image->depth != backend->depth ||
        stride > (size_t)INT_MAX / (size_t)backend->height ||
        xcb_get_image_data_length(image) < (int)(stride * backend->height)) {
        free(image);
        return VNCDESKTOP_CAPTURE_FAILED;
    }
    required = (size_t)backend->width * backend->height * 4;
    if (required > backend->capacity) {
        unsigned char *pixels = realloc(backend->pixels, required);
        if (!pixels) {
            free(image);
            return VNCDESKTOP_NO_MEMORY;
        }
        backend->pixels = pixels;
        backend->capacity = required;
    }
    source = xcb_get_image_data(image);
    for (y = 0; y < (size_t)backend->height; ++y) {
        if (backend->bytes_per_pixel == 4 &&
            backend->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST &&
            backend->red_mask == 0xff0000 &&
            backend->green_mask == 0xff00 && backend->blue_mask == 0xff) {
            unsigned char *output = backend->pixels + y * backend->width * 4;
            memcpy(output, source + y * stride, (size_t)backend->width * 4);
            /* The padding byte is not defined by X11; normalize it so it
             * cannot spuriously mark an unchanged framebuffer as modified. */
            for (x = 0; x < (size_t)backend->width; ++x)
                output[x * 4 + 3] = 0;
            continue;
        }
        for (x = 0; x < (size_t)backend->width; ++x) {
            const unsigned char *input = source + y * stride +
                                         x * backend->bytes_per_pixel;
            unsigned char *output = backend->pixels +
                                    (y * backend->width + x) * 4;
            uint32_t pixel = 0;
            unsigned int byte;
            for (byte = 0; byte < backend->bytes_per_pixel; ++byte) {
                unsigned int shift = backend->image_byte_order ==
                    XCB_IMAGE_ORDER_LSB_FIRST ? byte :
                    (unsigned int)backend->bytes_per_pixel - byte - 1;
                pixel |= (uint32_t)input[byte] << (shift * 8);
            }
            output[0] = component(pixel, backend->blue_mask);
            output[1] = component(pixel, backend->green_mask);
            output[2] = component(pixel, backend->red_mask);
            output[3] = 0;
        }
    }
    free(image);
    frame->width = backend->width;
    frame->height = backend->height;
    frame->pixels = backend->pixels;
    return VNCDESKTOP_OK;
}

static void fake_input(desktop_backend *backend, uint8_t type, uint8_t detail)
{
    xcb_test_fake_input(backend->connection, type, detail, XCB_CURRENT_TIME,
                        XCB_NONE, 0, 0, 0);
}

void desktop_pointer(desktop_backend *backend, int mask, int x, int y)
{
    unsigned int button, current = (unsigned int)mask & 0xff;
    if (backend->width <= 0 || backend->height <= 0)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= backend->width) x = backend->width - 1;
    if (y >= backend->height) y = backend->height - 1;
    xcb_test_fake_input(backend->connection, XCB_MOTION_NOTIFY, 0,
        XCB_CURRENT_TIME, backend->root, (int16_t)x, (int16_t)y, 0);
    for (button = 0; button < 8; ++button) {
        unsigned int bit = 1u << button;
        if ((backend->buttons ^ current) & bit)
            fake_input(backend, current & bit ? XCB_BUTTON_PRESS :
                       XCB_BUTTON_RELEASE, (uint8_t)(button + 1));
    }
    backend->buttons = current;
    xcb_flush(backend->connection);
}

static uint8_t find_key(desktop_backend *backend, uint32_t keysym,
                        int *shifted)
{
    const uint32_t *symbols = xcb_get_keyboard_mapping_keysyms(backend->keymap);
    unsigned int key, column, columns = backend->keymap->keysyms_per_keycode;
    /* Only the current base group and its shifted column are usable without
     * changing the user's keyboard layout. Unmapped keysyms are ignored. */
    for (column = 0; column < columns && column < 2; ++column) {
        for (key = backend->first_keycode; key <= backend->last_keycode; ++key) {
            if (symbols[(key - backend->first_keycode) * columns + column] ==
                keysym) {
                *shifted = column == 1;
                return (uint8_t)key;
            }
        }
    }
    return 0;
}

void desktop_key(desktop_backend *backend, int down, uint32_t keysym)
{
    uint8_t keycode = 0, shift_key = 0;
    unsigned int key;
    int shifted = 0, ignored = 0, have_shift = 0;
    if (!keysym || !backend->keymap)
        return;
    for (key = backend->first_keycode; key <= backend->last_keycode; ++key) {
        if (backend->held_keysym[key] == keysym)
            keycode = (uint8_t)key;
        if (backend->held_keysym[key] == 0xffe1 ||
            backend->held_keysym[key] == 0xffe2)
            have_shift = 1;
    }
    if (!down) {
        /* Some viewers report a different letter case when a modifier was
         * released before the letter. Still release the key we own. */
        if (!keycode) {
            uint8_t mapped = find_key(backend, keysym, &ignored);
            if (mapped && backend->held_keysym[mapped])
                keycode = mapped;
        }
        if (keycode) {
            fake_input(backend, XCB_KEY_RELEASE, keycode);
            backend->held_keysym[keycode] = 0;
            xcb_flush(backend->connection);
        }
        return;
    }
    {
        uint8_t mapped = find_key(backend, keysym, &shifted);
        if (!keycode)
            keycode = mapped;
    }
    if (!keycode)
        return;
    if (shifted && !have_shift) {
        shift_key = find_key(backend, 0xffe1, &ignored); /* Shift_L */
        if (shift_key)
            fake_input(backend, XCB_KEY_PRESS, shift_key);
    }
    fake_input(backend, XCB_KEY_PRESS, keycode);
    backend->held_keysym[keycode] = keysym;
    if (shift_key)
        fake_input(backend, XCB_KEY_RELEASE, shift_key);
    xcb_flush(backend->connection);
}

void desktop_release_inputs(desktop_backend *backend)
{
    unsigned int key, button;
    if (!backend || !backend->connection)
        return;
    for (key = 0; key < 256; ++key) {
        if (backend->held_keysym[key]) {
            fake_input(backend, XCB_KEY_RELEASE, (uint8_t)key);
            backend->held_keysym[key] = 0;
        }
    }
    for (button = 0; button < 8; ++button) {
        if (backend->buttons & (1u << button))
            fake_input(backend, XCB_BUTTON_RELEASE, (uint8_t)(button + 1));
    }
    backend->buttons = 0;
    xcb_flush(backend->connection);
}

void desktop_close(desktop_backend *backend)
{
    if (!backend)
        return;
    if (backend->connection) {
        desktop_release_inputs(backend);
        xcb_disconnect(backend->connection);
    }
    free(backend->keymap);
    free(backend->pixels);
    free(backend);
}
