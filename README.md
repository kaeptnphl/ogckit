# OGCKit — Open Game Controller Kit

Game controller input for console homebrew and emulators. PlayStation first, the core is platform independent.
Not related to libogc.

**Version: 0.1.0 (tentative).** This is an early proof of concept: the API, supported
controllers, and backend behavior may change. This is not a stable 1.0 release.
The [roadmap](ROADMAP.md) separates implemented work from plans and items that remain
unknown or unverified; it does not promise dates or releases.

- Plain C API (`include/ogckit.h`): `ogc_init()`, `ogc_get(slot, &info, &state)`.
- Hotplug by rescanning once per second, one reader thread per pad, thread-safe.
- Read-only: nothing is sent to a pad (no rumble yet).
- **Drivers** (understand a pad): XInput (Xbox 360 class, 8BitDo GBros. in XInput mode). Planned: Switch Pro, DualShock, generic HID.
- **Backends** (reach USB, one chosen at configure time with `OGC_BACKEND`): `ps4` (OpenOrbis/PacBrew,
  sceUsbd), `libusb` (Linux/macOS/Windows, for development; the default) and `iohid` (macOS, through the
  system HID driver, no root).
  Other platforms can add a `backend_*.cpp` (see `src/internal.h`). Bluetooth is a goal, not done.

Choose a backend explicitly for the intended target: `iohid` for macOS with the built-in HID driver,
`ps4` for the OpenOrbis/PacBrew PS4 toolchain, or `libusb` for Linux/macOS/Windows development where
libusb 1.0 development files are available. The default is `libusb`; it is not required for the
macOS IOHID or PS4 builds.

## Build and try (libusb)

Requires CMake, a C++17 compiler, pkg-config, and libusb 1.0 development files.

    cmake -S . -B build && cmake --build build
    ctest --test-dir build --output-on-failure
    build/ogc-terminal            # prints state changes of a connected pad

For a PS4 cross-build, use an installed OpenOrbis/PacBrew toolchain. Replace the
placeholder below with the toolchain's CMake toolchain-file path:

    cmake -S . -B build-ps4 -DOGC_BACKEND=ps4 -DCMAKE_TOOLCHAIN_FILE=/path/to/openorbis/cmake/ps4.cmake
    cmake --build build-ps4

The `ps4` backend has cross-built with OpenOrbis. That does not by itself establish
OGCKit's runtime compatibility on PS4; see the [PS4 notes](#ps4-notes).

## macOS IOHID backend

    cmake -S . -B build-iohid -DOGC_BACKEND=iohid && cmake --build build-iohid
    build-iohid/ogc-terminal

Links IOKit and CoreFoundation instead of libusb (consumers vendor `src/backend_iohid.cpp` and
`src/report_queue.h` in place of `src/backend_libusb.cpp`). Each scan enumerates HID devices with the
Generic Desktop/Gamepad primary usage that sit below a USB interface; the interface class triple is read
from that `IOUSBHostInterface`, so the unchanged drivers still decide what matches. Devices are opened
non-exclusively: macOS and other apps keep receiving the same input. Reports arrive on a per-device
dispatch queue and pass through a bounded queue (newest kept) to the reader; removal makes reads fail
after queued reports drain, and close waits for IOHID's cancel handler before releasing the device.
The read timeout only bounds how long a reader waits, which keeps stop/removal responsive; reports are
delivered as soon as they arrive. Open failures are logged with the IOReturn code.

Observed on one macOS setup (not a general guarantee): Apple's `XboxGamepad` driver exposes the GBros.
adapter (045E:028E) as a 20-byte input report without a report ID whose descriptor keeps the raw XInput
byte layout. A short normal-user run found the pad, received about 118 reports per second
and shut down cleanly twice. OGCTester, a separate application, later reported working input with
the macOS IOHID backend on that setup. This confirms that observed setup, not every device or OS
version. Other drivers or macOS versions may change the report; the XInput parser ignores reports it
does not recognize.

## Runtime and logging

There is one process-wide instance, not one instance per caller. Init/shutdown calls are serialized
and repeatable; any shutdown stops the instance. `ogc_get()` may run concurrently, including during
shutdown. It returns a coherent snapshot when connected, or zero without changing the caller's
outputs. Snapshots become unavailable when shutdown closes admission. State and sequence reset on
reconnection; a sequence number is not a device identity.

Shutdown first drains the scanner (including an in-flight open), then stops and joins readers before
resetting slots and closing the backend. It can wait for the current one-second scan sleep and USB
operations; an indefinitely stalled backend scan/open can still delay shutdown.

Reads use a 250 ms timeout, matching the functioning macOS tester configuration. This is not evidence
that the previous 8 ms timeout caused the reported input problem. Successful devices and read-error
recovery retain the normal scan cadence. Failed opens are retried after 30 seconds using a monotonic
clock and a bounded 16-key cache, reset on each successful init. A replug with the same bus/address key
can inherit that delay; when the cache is full, the oldest failure is replaced.

Set the optional logger before init or after shutdown. Changes while initialized are ignored and
reported to the current logger. Callbacks must be thread-safe and must not call init, shutdown, or
logger configuration; snapshot reads are allowed. Backend diagnostics use this callback (no hardwired
stderr), preserving operation names and native error codes, including interface-claim failures.

## Hardware-free tests

The parser, IOHID report-queue and synchronized fake-backend runtime tests do not require libusb or a
controller:

    cmake -S . -B build-tests -DOGC_BUILD_LIBRARY=OFF
    cmake --build build-tests
    ctest --test-dir build-tests --output-on-failure

The runtime suite covers repeated/concurrent lifecycle calls, init failures, slot limits, input state,
disconnect/reconnect, transport errors, open-failure recovery, retry timing beyond the former
unsigned-microsecond rollover, and shutdown while scan/open is held at a synchronization barrier.
All waits are bounded by test deadlines or CTest's runtime timeout. Parser tests reject truncated
reports whose advertised size exceeds the received data without changing the prior state.
For concurrency instrumentation with a supporting compiler, configure a separate build with
`-DCMAKE_CXX_FLAGS="-fsanitize=thread -g"` and `-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"`.

## Hardware observations

On one macOS setup, OGCTester reported working GameCube input through an 8BitDo GBros.
adapter in XInput mode with the IOHID backend. This is a separate application and a
setup-specific observation, not a general device or OS guarantee.

A separate PS4 application has also been run with a wired GBros. adapter in XInput mode.
This is evidence for that app and hardware setup only; it does not validate OGCKit's PS4
runtime or its `sceUsbd` integration. See the [PS4 notes](#ps4-notes).

## Validation notes

- sceUsbd reports libusb error `-N` as `0x80240000 + N`; an idle read is `0x80240007` (timeout).
- The native hardware-free XInput, core/runtime, and report-queue tests pass on Debian 13.
- The PS4 backend cross-build completes with the OpenOrbis toolchain; its static library contains
  FreeBSD-targeted x86-64 objects. The test executables produced by that cross-toolchain are not
  runnable on the Debian host; the tests were separately built and passed with the native compiler.
- The macOS IOHID backend was built on macOS; a separate OGCTester application reported working input
  on one setup. OGCTester is not built or tested as part of this repository.
- The complete libusb library build was not checked on the Mac because libusb development files are
  absent in that environment; this does not affect the separate PS4/OpenOrbis cross-build.

## PS4 notes

- The 8BitDo GBros. shows ONE pad in XInput mode, whatever is plugged in (GameCube, Classic, Nunchuk); the
  manufacturer supports one controller per adapter. Nunchuk: Z arrives as A, C as B, plus junk frames with all
  axes in a corner that a consumer should drop. Classic Controller triggers arrive as 0/255 only.
- The existing PS4 compatibility alias still frees the device list with `unref=0`. Under standard
  libusb ownership rules this retains device references and leaks them across scans. Whether that
  behavior applies to sceUsbd, and the exact sceUsbd ownership and ABI, remain unresolved: the public
  [OpenOrbis header](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/Usbd.h)
  declares `sceUsbdFreeDeviceList(list)` with one argument, unlike the existing two-argument alias,
  and gives no usable signatures for `sceUsbdRefDevice`/`sceUsbdUnrefDevice`. That declaration is not
  proof of the runtime implementation. The alias and unref policy are deliberately unchanged pending
  reliable ABI/ownership evidence and OGCKit PS4 runtime validation. The cross-build succeeds, but
  OGCKit's full runtime behavior on PS4 remains unverified.

## License

MIT, see `LICENSE`.
