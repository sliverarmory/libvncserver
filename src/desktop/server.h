/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VNCDESKTOP_SERVER_INTERNAL_H
#define VNCDESKTOP_SERVER_INTERNAL_H

#include <stdint.h>

/* Read worker state and its last error under one lock. Internal to vncdesktop. */
int32_t desktop_state(int32_t *last_status);

#endif
