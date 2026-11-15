/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VNCDESKTOP_BACKEND_H
#define VNCDESKTOP_BACKEND_H

#include <rfb/desktop.h>
#include <stddef.h>

typedef struct desktop_backend desktop_backend;
typedef struct desktop_frame {
    int width;
    int height;
    /* Tightly packed top-down B, G, R, unused bytes. Owned by the backend. */
    unsigned char *pixels;
} desktop_frame;

/* All backend calls occur on the same worker thread. On failure open cleans up
 * partial state and leaves *backend NULL. Capture may resize its owned buffer. */
int desktop_open(desktop_backend **backend);
int desktop_capture(desktop_backend *backend, desktop_frame *frame);
void desktop_pointer(desktop_backend *backend, int mask, int x, int y);
void desktop_key(desktop_backend *backend, int down, uint32_t keysym);
void desktop_release_inputs(desktop_backend *backend);
void desktop_close(desktop_backend *backend);

#endif
