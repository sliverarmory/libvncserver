/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RFB_DESKTOP_H
#define RFB_DESKTOP_H

#include <stdint.h>

#if defined(_WIN32)
# if defined(VNCDESKTOP_BUILD)
#  define VNCDESKTOP_API __declspec(dllexport)
# else
#  define VNCDESKTOP_API __declspec(dllimport)
# endif
# define VNCDESKTOP_CALL __cdecl
#else
# define VNCDESKTOP_API __attribute__((visibility("default")))
# define VNCDESKTOP_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum vncdesktop_status {
    VNCDESKTOP_OK = 0,
    VNCDESKTOP_INVALID_PORT = -1,
    VNCDESKTOP_ALREADY_RUNNING = -2,
    VNCDESKTOP_LISTEN_FAILED = -3,
    VNCDESKTOP_NO_DESKTOP = -4,
    VNCDESKTOP_PERMISSION_DENIED = -5,
    VNCDESKTOP_CAPTURE_FAILED = -6,
    VNCDESKTOP_NO_MEMORY = -7,
    VNCDESKTOP_THREAD_FAILED = -8
};

/* Start sharing the current desktop on 127.0.0.1:port (1..65535).
 * Returns 0 once ready, or a negative vncdesktop_status; runs in a worker thread.
 * One server per loaded library. There is no VNC password: local users can connect.
 */
VNCDESKTOP_API int32_t VNCDESKTOP_CALL start(int32_t port);
/* Disconnect clients and join the worker. Safe to call repeatedly.
 * Call before unloading the library. */
VNCDESKTOP_API void VNCDESKTOP_CALL vnc_stop(void);
/* Current/last worker status, including asynchronous capture failures. */
VNCDESKTOP_API int32_t VNCDESKTOP_CALL vnc_status(void);

#ifdef __cplusplus
}
#endif
#endif
