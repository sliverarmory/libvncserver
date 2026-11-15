# Local desktop shared library

The **Desktop shared libraries** GitHub Actions workflow builds a standalone
`vncdesktop` library with a C ABI on pushes, pull requests, and manual dispatch.
Download the `vncdesktop-<platform>-<commit>` artifact from the workflow run:

| Platform | Library | Requirements |
| --- | --- | --- |
| macOS Intel / Apple silicon | `lib/libvncdesktop.dylib` | macOS 14+, active desktop, Screen Recording and Accessibility permissions |
| Windows x64 | `bin/vncdesktop.dll` | Interactive logged-in desktop; MSVC runtime statically linked |
| Linux x86_64 / arm64 | `lib/libvncdesktop.so` | glibc 2.39+ (Ubuntu 24.04 baseline), X11, `libxcb` and `libxcb-xtest` |

The VNC core is statically embedded. No separate `libvncserver` library is needed.
Artifacts include `include/rfb/desktop.h`, this guide, the GPL license, and
`manifest.json` with the source commit and SHA-256 hashes of installed files.
The library is GPL-2.0-or-later, like LibVNCServer.

## GitHub Releases

The **Publish desktop release** workflow runs when an annotated tag matching
`desktop-vX.Y.Z` is pushed. It requires the tagged commit to be on `master`, runs
the same five build/test jobs as CI, and publishes only after every job succeeds.
An existing tag can also be selected using the workflow's **Run workflow** menu,
or `gh workflow run publish-desktop.yml --ref desktop-v0.1.0`.

To publish a new version after committing and pushing the desired source:

```sh
git tag -s desktop-v0.1.0 -m "Desktop shared libraries 0.1.0"
git push origin desktop-v0.1.0
```

Choose an unused version for subsequent releases. A published release is never
overwritten by the workflow; use a new version for changes. A failed upload can
resume an existing draft for the same tag and commit.

Release downloads contain four `.tar.gz` archives (macOS/Linux, Intel/ARM), one
Windows `.zip`, `SHA256SUMS`, and `release.json`. Each archive includes the shared
library, header, license, guide, and manifest. The publisher verifies all five
platforms against the tagged commit and checks every installed file's hash,
then downloads the uploaded assets and compares them byte-for-byte before making
the draft public. The release notes link to the exact source and Actions run;
GitHub's source archives provide the corresponding tagged source.

After downloading the release assets into one directory, verify them with:

```sh
sha256sum --check SHA256SUMS
# macOS: shasum -a 256 --check SHA256SUMS
```

## API

```c
#include <rfb/desktop.h>

int32_t status = start(1234);
if (status == VNCDESKTOP_OK) {
    /* The desktop is now available at 127.0.0.1:1234.
       The caller can continue doing other work. */
}
/* Later, before unloading the library: */
vnc_stop();
```

`start(int32_t port)` returns zero after the desktop has been captured and the
listener has opened. Capture, VNC traffic, and keyboard/mouse input run in an
internal worker thread. The port is the exact TCP port, not a VNC display number;
for a viewer using `host::port` syntax, connect to `127.0.0.1::1234`.
The exported name is exactly `start`, with C linkage and the `cdecl` calling
convention on Windows.

There is one server and one controlling client per loaded library. A second
`start` returns `-2`; call `vnc_stop()` before restarting, including after an
asynchronous failure. `vnc_stop()` disconnects clients, releases held input,
joins the worker, and can be called repeatedly. Never unload the library while
it is running. Do not call these APIs from `DllMain`, a signal handler, or a
loader callback.

`vnc_status()` returns the current/last worker error, including capture failures
after startup. Zero means no error recorded; it is not a running-state indicator.
Startup waits for the OS capture service, and shutdown waits for any in-flight
capture and client I/O to finish. In particular, an unresponsive macOS capture
service can delay these calls; callbacks are drained before allowing unload.

| Value | Meaning |
| --- | --- |
| `0` | Success |
| `-1` | Port outside 1–65535 |
| `-2` | Server already started; stop before restarting |
| `-3` | Requested listener could not be opened, e.g. port in use |
| `-4` | No supported desktop/session available |
| `-5` | Required desktop permissions missing |
| `-6` | Desktop capture failed |
| `-7` | Allocation failed |
| `-8` | Worker thread creation failed |

Load from Python, for example:

```python
import ctypes

library = ctypes.CDLL("/absolute/path/to/libvncdesktop.so")  # or .dylib / .dll
library.start.argtypes = [ctypes.c_int32]
library.start.restype = ctypes.c_int32
library.vnc_status.argtypes = []
library.vnc_status.restype = ctypes.c_int32
library.vnc_stop.argtypes = []
library.vnc_stop.restype = None
status = library.start(1234)
if status:
    raise RuntimeError(f"VNC startup failed: {status}")
try:
    input("VNC running; press Enter to stop... ")
finally:
    library.vnc_stop()
```

## Desktop behavior and access

The listener binds only IPv4 loopback, `127.0.0.1`. IPv6, HTTP, UDP, file transfer,
and port auto-selection are disabled. There is no VNC password or TLS; other
processes/users on the same host can view and control this desktop while it runs.
The build profile disables optional compression dependencies, while retaining
the core raw, RRE, CoRRE, Hextile, and Ultra encodings.

- **macOS:** captures the main display at its native pixel resolution. Grant
  Screen Recording and Accessibility permissions to the application/process
  loading the library in System Settings → Privacy & Security, then restart
  that host application. Startup returns `-5` without prompting if either grant
  is absent. Text uses Unicode injection; modified shortcuts use ANSI key positions.
- **Windows:** captures the virtual desktop, including multiple monitors.
  Run inside the user's interactive session. Session 0 services and secure
  desktops are not supported. Windows integrity restrictions still apply to
  controlling elevated applications. A desktop switch stops capture.
- **Linux:** shares the root screen selected by `DISPLAY`, using the caller's
  X11 authorization (`XAUTHORITY`, if needed). XTEST must be enabled. Wayland and
  Xwayland-only sessions return `-4`; log into an X11 session. Keyboard mapping
  uses the first X11 layout group. Install runtime dependencies on Debian/Ubuntu
  with `sudo apt-get install libxcb1 libxcb-xtest0`.

Display size changes use the VNC desktop-resize extension. Clipboard, audio,
and file transfer are not implemented. The server uses the VNC cursor, and
releases injected keys/buttons when the client disconnects.

## Build and validate locally

Use a fresh build directory with CMake 3.16+, a C compiler, and Python 3.8+ for tests.
macOS additionally needs Xcode command-line tools; Windows builds use MSVC.
Linux build dependencies are `libxcb1-dev`, `libxcb-xtest0-dev`, and `pkg-config`.

```sh
cmake -S . -B build/desktop -C cmake/DesktopBuild.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure -R '^desktop_'
cmake --install build/desktop --config Release --component desktop --prefix "$PWD/dist"
```

On headless Linux, run tests under `xvfb-run -a` (packages `xvfb` and `xauth`).
The workflow requires a native desktop framebuffer exchange on Linux/Xvfb and
Windows, plus native capture/input tests in dedicated X11 and Windows test windows.
On macOS, hosted runners may lack
privacy grants: native capture is explicitly marked skipped in that case.
Every platform still runs exported ABI, listener, full VNC handshake,
framebuffer, resize, input-forwarding, and shutdown checks against a separate
test fixture. That fixture is never installed or included in artifacts.
To require a real desktop test locally, run `test/desktop_smoke.py` against the
installed library without `--allow-unavailable`.
