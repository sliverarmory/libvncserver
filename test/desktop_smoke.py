#!/usr/bin/env python3
"""Load the public ABI and exercise a real RFB session with no third-party modules.

Native runs never send keyboard or mouse events. --fixture enables deterministic
pixel, input forwarding, resize, and capture failure checks on the test library.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
import os
import socket
import struct
import sys
import threading
import time
from pathlib import Path


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def receive(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise AssertionError(f"RFB peer closed with {count - len(data)} bytes missing")
        data.extend(part)
    return bytes(data)


def unused_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def refused(address, family=socket.AF_INET):
    with socket.socket(family, socket.SOCK_STREAM) as sock:
        sock.settimeout(1)
        try:
            sock.connect(address)
        except OSError:
            return True
        return False


def check_loopback_only(port):
    if socket.has_ipv6:
        check(refused(("::1", port), socket.AF_INET6),
              f"Unexpected IPv6 listener on ::1:{port}")
    addresses = set()
    # UDP connect chooses the outgoing local address without sending a packet.
    # Avoid resolving gethostname(): on hosts without a matching DNS record that
    # can stall for tens of seconds and does not reliably enumerate interfaces.
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
            route.connect(("192.0.2.1", 9))
            addresses.add(route.getsockname()[0])
    except OSError:
        pass
    addresses = {address for address in addresses
                 if not address.startswith("127.") and address != "0.0.0.0"}
    for address in addresses:
        check(refused((address, port)),
              f"VNC unexpectedly accepts non-loopback connections on {address}:{port}")
    print(f"Loopback isolation verified ({len(addresses)} non-loopback addresses probed)", flush=True)


class RFB:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        try:
            check(receive(self.sock, 12) == b"RFB 003.008\n", "Expected RFB 3.8 banner")
            self.sock.sendall(b"RFB 003.008\n")
            count = receive(self.sock, 1)[0]
            check(count > 0, "Server rejected authentication negotiation")
            security = receive(self.sock, count)
            check(1 in security, f"No None security type in {security!r}")
            self.sock.sendall(b"\x01")
            check(struct.unpack("!I", receive(self.sock, 4))[0] == 0,
                  "None authentication failed")
            self.sock.sendall(b"\x01")  # shared request; server still limits clients
            header = receive(self.sock, 24)
            self.width, self.height = struct.unpack("!HH", header[:4])
            pixel_format = struct.unpack("!BBBBHHHBBB3x", header[4:20])
            # LibVNCServer's TRUE is -1, serialized as 255. RFB booleans use
            # zero/nonzero semantics, so accept either 1 or 255 here.
            normalized_format = pixel_format[:3] + (bool(pixel_format[3]),) + pixel_format[4:]
            check(normalized_format == (32, 24, 0, True, 255, 255, 255, 16, 8, 0),
                  f"Unexpected server pixel format {pixel_format}")
            name_length = struct.unpack("!I", header[20:])[0]
            check(name_length <= 1024, "Excessive desktop name")
            self.name = receive(self.sock, name_length).decode("utf-8")
            check(0 < self.width <= 16384 and 0 < self.height <= 16384,
                  f"Invalid desktop size {self.width}x{self.height}")
            self.pixels = bytearray(self.width * self.height * 4)
            self.saw_raw = False
            self.saw_resize = False
            # Request separate cursor updates to keep cursor compositing out of
            # the deterministic framebuffer, plus desktop-resize notifications.
            encodings = (0, -223, -239, -232)
            self.sock.sendall(struct.pack("!BBH4i", 2, 0, len(encodings), *encodings))
        except BaseException:
            self.sock.close()
            raise

    def close(self):
        self.sock.close()

    def request(self):
        self.sock.sendall(struct.pack("!BBHHHH", 3, 0, 0, 0, self.width, self.height))

    def update(self):
        message = receive(self.sock, 1)[0]
        check(message == 0, f"Unexpected RFB server message type {message}")
        _, rectangles = struct.unpack("!BH", receive(self.sock, 3))
        for _ in range(rectangles):
            x, y, width, height, encoding = struct.unpack("!HHHHi", receive(self.sock, 12))
            if encoding == -223:
                check(0 < width <= 16384 and 0 < height <= 16384, "Invalid resize")
                self.width, self.height = width, height
                self.pixels = bytearray(width * height * 4)
                self.saw_resize = True
                self.saw_raw = False
            elif encoding == -239:
                receive(self.sock, width * height * 4 + ((width + 7) // 8) * height)
            elif encoding == -232:
                pass  # pointer position, no payload
            else:
                check(encoding == 0, f"Expected raw pixels, got encoding {encoding}")
                check(x + width <= self.width and y + height <= self.height,
                      "Raw rectangle outside framebuffer")
                data = receive(self.sock, width * height * 4)
                for row in range(height):
                    offset = ((y + row) * self.width + x) * 4
                    self.pixels[offset:offset + width * 4] = data[row * width * 4:(row + 1) * width * 4]
                self.saw_raw = True

    def until(self, predicate, description):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.request()
            self.update()
            if predicate():
                return
            time.sleep(0.04)
        raise AssertionError(f"Timed out waiting for {description}")

    def key(self, keysym):
        self.sock.sendall(struct.pack("!BB2xI", 4, 1, keysym))
        self.sock.sendall(struct.pack("!BB2xI", 4, 0, keysym))

    def pointer(self, mask, x, y):
        self.sock.sendall(struct.pack("!BBHH", 5, mask, x, y))

    def pixel(self, x, y):
        offset = (y * self.width + x) * 4
        return self.pixels[offset:offset + 4]


def expected_pixels(width, height, pointer=False, typed=False):
    data = bytearray(component
                     for y in range(height)
                     for x in range(width)
                     for component in (x, y, (x + y) & 255, 0))
    if pointer:
        offset = (7 * width + 5) * 4
        data[offset:offset + 4] = b"\x11\x22\x33\0"
    if typed:
        data[:4] = b"\x44\x55\x66\0"
    return data


def run(args):
    path = Path(args.library).resolve(strict=True)
    # Explicit search directory also supports builds using a neighboring core
    # DLL. Release artifacts normally embed the static core into vncdesktop.
    dll_directory = os.add_dll_directory(str(path.parent)) if os.name == "nt" else None
    lib = ctypes.CDLL(str(path))
    lib.start.argtypes = (ctypes.c_int32,)
    lib.start.restype = ctypes.c_int32
    lib.vnc_stop.argtypes = ()
    lib.vnc_stop.restype = None
    lib.vnc_status.argtypes = ()
    lib.vnc_status.restype = ctypes.c_int32
    client = None
    try:
        for port in (0, -1, 65536, -2147483648, 2147483647):
            check(lib.start(port) == -1, f"Invalid port {port} was not rejected")
        lib.vnc_stop()
        lib.vnc_stop()

        port = unused_port()
        status = lib.start(port)
        if args.allow_unavailable and status in (-4, -5):
            check(refused(("127.0.0.1", port)), "Unavailable desktop left a listener open")
            check(lib.vnc_status() == status, "Unavailable desktop status was lost")
            print(f"SKIP: native desktop unavailable or permission denied (status {status})", flush=True)
            return 77
        check(status == 0, f"start({port}) failed with status {status}")
        check(lib.vnc_status() == 0, "Unexpected startup status")
        check(lib.start(port) == -2, "Duplicate start did not return ALREADY_RUNNING")
        check_loopback_only(port)
        client = RFB(port)
        client.until(lambda: client.saw_raw, "initial raw framebuffer")
        print(f"RFB 3.8 handshake and raw framebuffer passed: {client.width}x{client.height}", flush=True)

        if args.fixture:
            check((client.width, client.height) == (64, 48), "Wrong fixture size")
            check(client.pixels == expected_pixels(64, 48), "Fixture pixels differ")
            client.pointer(1, 5, 7)
            client.until(lambda: client.pixel(5, 7) == b"\x11\x22\x33\0", "pointer forwarding")
            # A refused connection must not release the active controller's
            # held buttons. LibVNCServer sends its banner before newClientHook.
            with socket.create_connection(("127.0.0.1", port), timeout=3) as second:
                banner = bytearray()
                while True:
                    try:
                        part = second.recv(64)
                    except ConnectionResetError:
                        break
                    if not part:
                        break
                    banner.extend(part)
                    check(len(banner) <= 12, "Second controller unexpectedly accepted")
            time.sleep(0.1)  # permit capture after the refused client's cleanup
            client.request()
            client.update()
            check(client.pixel(5, 7) == b"\x11\x22\x33\0",
                  "Refused connection released the active controller's button")
            client.key(ord("x"))
            client.until(lambda: client.pixel(0, 0) == b"\x44\x55\x66\0", "keyboard forwarding")
            client.key(0xFFC8)  # XK_F11
            resized_pixels = expected_pixels(80, 60, pointer=True, typed=True)
            client.until(lambda: client.saw_resize and client.saw_raw and
                         (client.width, client.height) == (80, 60) and
                         client.pixels == resized_pixels, "complete resized framebuffer")
            client.key(0xFFC9)  # XK_F12
            deadline = time.monotonic() + 5
            while lib.vnc_status() == 0 and time.monotonic() < deadline:
                time.sleep(0.02)
            check(lib.vnc_status() == -6, "Asynchronous capture failure was not published")
            print("Exact pixels, input forwarding, resize, and capture-failure status passed", flush=True)

        lib.vnc_stop()
        lib.vnc_stop()
        # Resize notifications and cursor changes can leave a requested update
        # queued. Drain bytes already sent, then require EOF/reset after stop.
        deadline = time.monotonic() + 3
        while True:
            check(time.monotonic() < deadline, "Client stayed open after stop")
            client.sock.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                if not client.sock.recv(65536):
                    break
            except ConnectionResetError:
                break
        client.close()
        client = None
        check(refused(("127.0.0.1", port)), "Listener stayed open after stop")

        with socket.socket() as occupied:
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            occupied.bind(("127.0.0.1", 0))
            occupied.listen(1)
            blocked_port = occupied.getsockname()[1]
            check(lib.start(blocked_port) == -3, "Occupied port did not return LISTEN_FAILED")
            check(lib.vnc_status() == -3, "Listen failure status was lost")

        check(lib.start(port) == 0, "Restart failed")
        client = RFB(port)
        client.until(lambda: client.saw_raw, "framebuffer after restart")
        if args.fixture:
            check((client.width, client.height) == (64, 48), "Restart retained old dimensions")
            check(client.pixels == expected_pixels(64, 48), "Restart retained old pixel state")
        check(lib.vnc_status() == 0, "Restart retained error status")
        lib.vnc_stop()
        if args.fixture:
            # FFI callers may race startup/shutdown from multiple host threads.
            # Exactly one start owns the server; every stop waits for cleanup.
            barrier = threading.Barrier(4)

            def competing_start(_):
                barrier.wait(timeout=5)
                return lib.start(port)

            with ThreadPoolExecutor(max_workers=4) as pool:
                statuses = list(pool.map(competing_start, range(4)))
                check(sorted(statuses) == [-2, -2, -2, 0],
                      f"Concurrent starts returned {statuses}")
                list(pool.map(lambda _: lib.vnc_stop(), range(4)))
            check(refused(("127.0.0.1", port)), "Concurrent stops left a listener")
            print("Concurrent start/stop passed", flush=True)
        print("Invalid ports, duplicate start, stop, occupied port, and restart passed", flush=True)
        return 0
    finally:
        if client is not None:
            client.close()
        lib.vnc_stop()
        if dll_directory is not None:
            dll_directory.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library")
    parser.add_argument("--fixture", action="store_true")
    parser.add_argument("--allow-unavailable", action="store_true")
    sys.exit(run(parser.parse_args()))
