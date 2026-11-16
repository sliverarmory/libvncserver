#!/usr/bin/env python3
"""Exercise the Sliver export against the deterministic VNC desktop fixture."""

import ctypes
import os
import socket
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
from desktop_smoke import RFB, check, refused, unused_port


CALLBACK = (ctypes.WINFUNCTYPE if os.name == "nt" else ctypes.CFUNCTYPE)(
    ctypes.c_int32, ctypes.c_void_p, ctypes.c_int32
)


def run(path):
    library_path = Path(path).resolve(strict=True)
    dll_directory = os.add_dll_directory(str(library_path.parent)) if os.name == "nt" else None
    library = ctypes.CDLL(str(library_path), use_last_error=(os.name == "nt"))
    library.VNCDesktopCommand.argtypes = (ctypes.c_void_p, ctypes.c_uint32, CALLBACK)
    library.VNCDesktopCommand.restype = ctypes.c_int32
    library.vnc_running.argtypes = ()
    library.vnc_running.restype = ctypes.c_int32

    def command(data, expected_status, expected_text):
        outputs = []

        def on_output(pointer, length):
            outputs.append(ctypes.string_at(pointer, length).decode("utf-8"))
            return 0

        callback = CALLBACK(on_output)
        buffer = ctypes.create_string_buffer(data) if data else None
        pointer = ctypes.cast(buffer, ctypes.c_void_p) if buffer is not None else None
        if os.name == "nt":
            ctypes.set_last_error(1234)
        result = library.VNCDesktopCommand(pointer, len(data), callback)
        if os.name == "nt":
            check(ctypes.get_last_error() == 0,
                  f"{data!r}: adapter left a stale Win32 last error")
        if expected_status is not None:
            check(result == expected_status, f"{data!r}: status {result}, expected {expected_status}")
        check(len(outputs) == 1, f"{data!r}: expected one synchronous callback, got {outputs!r}")
        if expected_text is not None:
            check(expected_text in outputs[0], f"{data!r}: unexpected output {outputs[0]!r}")
        return outputs[0]

    try:
        check(library.vnc_running() == 0, "Fixture unexpectedly running before start")
        command(b"status", 0, "VNC stopped; last status 0")
        for data in (b"", b"start", b"start --port"):
            command(data, -1, "Usage:")
        for data in (b"start --port 0", b"start --port 65536", b"start --port -1",
                     b"start --port 12x", b"start --port 1234 ",
                     b"start --port 999999999999999999999",
                     b"start --port 1234\x00suffix", b"x" * 129):
            command(data, -1, "Invalid")
        check(library.vnc_running() == 0, "Invalid input started the fixture")

        port = unused_port()
        command(f"start --port {port}".encode(), 0, f"127.0.0.1:{port}")
        check(library.vnc_running() == 1, "Start did not mark worker as running")
        client = RFB(port)
        try:
            client.until(lambda: client.saw_raw, "initial framebuffer through Sliver adapter")
            command(b"status", 0, "VNC running; last status 0")
            command(f"start --port {port}".encode(), -2, "already running")
        finally:
            client.close()

        command(b"stop", 0, "VNC stopped")
        check(library.vnc_running() == 0, "Stop left worker marked as running")
        check(refused(("127.0.0.1", port)), "Stop left listener open")
        command(b"status", 0, "VNC stopped; last status 0")
        command(b"stop", 0, "VNC stopped")

        with socket.socket() as occupied:
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            occupied.bind(("127.0.0.1", 0))
            occupied.listen(1)
            blocked_port = occupied.getsockname()[1]
            command(f"start --port {blocked_port}".encode(), -3,
                    "could not open the loopback listener")
            command(b"status", -3, "VNC stopped; last status -3")

        command(f"start --port {port}".encode(), 0, f"127.0.0.1:{port}")
        client = RFB(port)
        try:
            client.key(0xFFC9)  # Fixture F12 simulates an asynchronous capture error.
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                output = command(b"status", None, "VNC")
                if "last status -6" in output:
                    break
                time.sleep(0.02)
            else:
                raise AssertionError("Asynchronous capture failure was not reported")
            check(library.vnc_running() == 0, "Failed worker still marked running")
        finally:
            client.close()
        command(b"stop", 0, "VNC stopped")

        def reject_output(_pointer, _length):
            return 1

        rejected_callback = CALLBACK(reject_output)
        data = f"start --port {port}".encode()
        buffer = ctypes.create_string_buffer(data)
        if os.name == "nt":
            ctypes.set_last_error(1234)
        result = library.VNCDesktopCommand(ctypes.cast(buffer, ctypes.c_void_p),
                                           len(data), rejected_callback)
        if os.name == "nt":
            check(ctypes.get_last_error() == 0,
                  "Rejected callback left a stale Win32 last error")
        check(result == -1, "Rejected callback did not fail the command")
        check(refused(("127.0.0.1", port)), "Rejected callback left a listener open")
        print("Sliver command ABI, port parsing, RFB, lifecycle, and async status passed", flush=True)
        return 0
    finally:
        library.vnc_stop()
        if dll_directory is not None:
            dll_directory.close()


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
