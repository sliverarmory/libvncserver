/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only deterministic backend; never linked into the shipped library. */
#include "backend.h"
#include <rfb/keysym.h>
#include <stdlib.h>

struct desktop_backend {
    unsigned char pixels[80 * 60 * 4];
    int width, height;
    int pointer_x, pointer_y, pointer_pressed, typed, failed;
};

int desktop_open(desktop_backend **backend)
{
    *backend = calloc(1, sizeof(**backend));
    if (!*backend)
        return VNCDESKTOP_NO_MEMORY;
    (*backend)->width = 64;
    (*backend)->height = 48;
    return VNCDESKTOP_OK;
}

int desktop_capture(desktop_backend *backend, desktop_frame *frame)
{
    int x, y;
    if (backend->failed)
        return VNCDESKTOP_CAPTURE_FAILED;
    for (y = 0; y < backend->height; ++y) {
        for (x = 0; x < backend->width; ++x) {
            unsigned char *pixel = backend->pixels + (y * backend->width + x) * 4;
            pixel[0] = (unsigned char)x;
            pixel[1] = (unsigned char)y;
            pixel[2] = (unsigned char)(x + y);
            pixel[3] = 0;
        }
    }
    if (backend->pointer_pressed) {
        unsigned char *pixel = backend->pixels +
            (backend->pointer_y * backend->width + backend->pointer_x) * 4;
        pixel[0] = 0x11; pixel[1] = 0x22; pixel[2] = 0x33;
    }
    if (backend->typed) {
        backend->pixels[0] = 0x44;
        backend->pixels[1] = 0x55;
        backend->pixels[2] = 0x66;
    }
    frame->width = backend->width;
    frame->height = backend->height;
    frame->pixels = backend->pixels;
    return VNCDESKTOP_OK;
}

void desktop_pointer(desktop_backend *backend, int mask, int x, int y)
{
    if (x < 0 || y < 0 || x >= backend->width || y >= backend->height)
        return;
    backend->pointer_x = x;
    backend->pointer_y = y;
    backend->pointer_pressed = mask & 1;
}

void desktop_key(desktop_backend *backend, int down, uint32_t keysym)
{
    if (!down)
        return;
    if (keysym == XK_F11) {
        backend->width = 80;
        backend->height = 60;
    } else if (keysym == XK_F12) {
        backend->failed = 1;
    } else if (keysym == 'x') {
        backend->typed = 1;
    }
}

void desktop_release_inputs(desktop_backend *backend)
{
    backend->pointer_pressed = 0;
}

void desktop_close(desktop_backend *backend)
{
    free(backend);
}
