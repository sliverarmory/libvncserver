/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rfb/desktop.h>
#include "server.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* Sliver joins native extension arguments with spaces before calling an export.
 * Keep the accepted grammar small so the port cannot be confused with another
 * command or silently truncated by an integer conversion. */
#define MAX_COMMAND_LENGTH 128

static int32_t emit(vncdesktop_output_callback callback, char *message,
                    int32_t status)
{
    if (!callback || callback(message, (int32_t)strlen(message)) != 0)
        return -1;
    return status;
}

static int32_t finish(int32_t status)
{
#ifdef _WIN32
    /* Sliver checks GetLastError after calling the export, even when it got
     * callback output. OS capture and the callback may leave a stale value. */
    SetLastError(ERROR_SUCCESS);
#endif
    return status;
}

static const char *status_text(int32_t status)
{
    switch (status) {
    case VNCDESKTOP_OK: return "no error recorded";
    case VNCDESKTOP_INVALID_PORT: return "invalid port";
    case VNCDESKTOP_ALREADY_RUNNING: return "already running; stop before restarting";
    case VNCDESKTOP_LISTEN_FAILED: return "could not open the loopback listener";
    case VNCDESKTOP_NO_DESKTOP: return "no supported desktop session";
    case VNCDESKTOP_PERMISSION_DENIED: return "desktop permission denied";
    case VNCDESKTOP_CAPTURE_FAILED: return "desktop capture failed";
    case VNCDESKTOP_NO_MEMORY: return "allocation failed";
    case VNCDESKTOP_THREAD_FAILED: return "worker thread creation failed";
    default: return "unknown error";
    }
}

static int parse_port(const char *digits, int32_t *port)
{
    uint32_t value = 0;
    if (!*digits) return 0;
    for (; *digits; ++digits) {
        if (*digits < '0' || *digits > '9') return 0;
        value = value * 10 + (uint32_t)(*digits - '0');
        if (value > 65535) return 0;
    }
    if (value == 0) return 0;
    *port = (int32_t)value;
    return 1;
}

int32_t VNCDESKTOP_CALL VNCDesktopCommand(const char *arguments,
                                          uint32_t argument_length,
                                          vncdesktop_output_callback callback)
{
    static const char start_prefix[] = "start --port ";
    char command[MAX_COMMAND_LENGTH + 1];
    char message[192];
    int32_t status, port, emitted, active;

    if (!callback) return finish(-1);
    if (argument_length > MAX_COMMAND_LENGTH ||
        (argument_length > 0 && !arguments))
        return finish(emit(callback, "Invalid vnc arguments\n", -1));
    if (argument_length > 0 && memchr(arguments, '\0', argument_length))
        return finish(emit(callback, "Invalid vnc arguments\n", -1));

    if (argument_length) memcpy(command, arguments, argument_length);
    command[argument_length] = '\0';

    if (strncmp(command, start_prefix, sizeof(start_prefix) - 1) == 0) {
        if (!parse_port(command + sizeof(start_prefix) - 1, &port))
            return finish(emit(callback, "Invalid port; use vnc start --port <1-65535>\n", -1));

        status = start(port);
        if (status == VNCDESKTOP_OK)
            snprintf(message, sizeof(message), "VNC listening on 127.0.0.1:%d\n", (int)port);
        else
            snprintf(message, sizeof(message), "VNC start failed (%d): %s\n",
                     (int)status, status_text(status));
        emitted = emit(callback, message, status);
        if (emitted == -1 && status == VNCDESKTOP_OK) {
            /* Do not leave an unacknowledged, unauthenticated listener open. */
            vnc_stop();
            return finish(-1);
        }
        return finish(emitted);
    }

    if (strcmp(command, "status") == 0) {
        active = desktop_state(&status);
        snprintf(message, sizeof(message), "VNC %s; last status %d (%s)\n",
                 active ? "running" : "stopped", (int)status,
                 status_text(status));
        return finish(emit(callback, message, status));
    }

    if (strcmp(command, "stop") == 0) {
        vnc_stop();
        return finish(emit(callback, "VNC stopped\n", VNCDESKTOP_OK));
    }

    return finish(emit(callback,
                       "Usage: vnc start --port <1-65535> | vnc status | vnc stop\n", -1));
}
