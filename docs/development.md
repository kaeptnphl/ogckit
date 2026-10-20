# Developing with OGCKit

OGCKit is an early proof of concept. Start with the [README](../README.md) for its
purpose and current scope, or the [roadmap](../ROADMAP.md) for intended directions.
This guide records integration behavior, backend caveats, and validation evidence;
it is not a general compatibility guarantee.

## Build and backend selection

Builds require CMake and a C++17 compiler. Choose one backend at configure time:

| Backend | Intended use | Additional requirements |
|---|---|---|
| `libusb` (default) | Linux/macOS/Windows development | pkg-config and libusb 1.0 development files |
| `iohid` | macOS system HID driver | IOKit and CoreFoundation; no libusb |
| `ps4` | PS4 cross-build | OpenOrbis/PacBrew toolchain; no libusb development package |

Desktop example:

```sh
cmake -S . -B build -DOGC_BACKEND=libusb
cmake --build build
build/ogc-terminal
```

For IOHID, use a separate build directory:

```sh
cmake -S . -B build-iohid -DOGC_BACKEND=iohid
cmake --build build-iohid
build-iohid/ogc-terminal
```

For PS4, replace the placeholder with your installed toolchain-file path:

```sh
cmake -S . -B build-ps4 -DOGC_BACKEND=ps4 -DCMAKE_TOOLCHAIN_FILE=/path/to/openorbis/cmake/ps4.cmake
cmake --build build-ps4
```

The PS4 target produces a static library, not the desktop terminal example.
Cross-build success alone is not runtime validation. Consumers vendoring sources
select the corresponding `src/backend_*.cpp`; IOHID also needs `src/report_queue.h`.
The backend contract for adding a platform is in [`src/internal.h`](../src/internal.h).

## API and lifecycle

The plain C interface is defined in [`include/ogckit.h`](../include/ogckit.h).
Call `ogc_init()` (zero means success; negative means failure), read slots with
`ogc_get(slot, &info, &state)`, then call `ogc_shutdown()`. Slots range from zero to
`OGC_MAX_PADS - 1`. Input is read-only; no rumble or other pad output is implemented.

There is one process-wide instance, not one per caller or a reference-counted
instance. Init/shutdown are serialized and repeatable; any shutdown stops it.
`ogc_get()` may run concurrently, including during shutdown. It returns a coherent
snapshot when connected, or zero without changing the caller's outputs. Snapshots
become unavailable when shutdown closes admission. State and sequence reset on
reconnection; sequence is not a device identity.

Hotplug uses a scan about once per second and one reader thread per pad. Shutdown
drains the scanner, including an in-flight open, then stops and joins readers
before resetting slots and closing the backend. It can wait for the scan sleep
and USB operations; an indefinitely stalled scan/open can delay shutdown.

Reads use a 250 ms timeout. This value is not evidence that an earlier timeout
caused input problems. Successful devices and read-error recovery retain the
normal scan cadence. Failed opens are retried after 30 seconds using a monotonic
clock and a bounded 16-key cache, reset on successful init. A replug with the same
bus/address key can inherit that delay; the oldest failure is replaced when full.

## Logging

Set the optional `ogc_set_log()` callback before init or after shutdown. Changes
while initialized are ignored and reported to the current logger. Callbacks can
run concurrently on library and calling threads: they must be thread-safe and
must not call init, shutdown, or logger configuration. Snapshot reads are allowed.
Backend diagnostics use this callback rather than hardwired stderr, preserving
operation names and native error codes, including interface-claim failures.

## macOS IOHID backend

IOHID enumerates Generic Desktop/Gamepad devices below a USB interface. Interface
class information comes from `IOUSBHostInterface`; protocol drivers still decide
what matches. Devices open non-exclusively, so macOS and other apps retain input.
Reports arrive on a per-device dispatch queue and enter a bounded queue that keeps
the newest reports. Removal makes reads fail after queued reports drain; close
waits for IOHID's cancel handler before releasing the device.

The read timeout bounds waiting, not report delivery latency. Reports are
delivered as they arrive. Open failures are logged with the IOReturn code.

## Hardware evidence

The GBros. adapter is a versatile initial test platform, not the library's scope.
The following observations establish only the paths and setups described.

### macOS

On one setup, Apple's `XboxGamepad` driver exposed GBros. (`045E:028E`) as a
20-byte report without a report ID, retaining the raw XInput byte layout. A short
normal-user IOHID run received about 118 reports per second and shut down cleanly
twice. OGCTester later reported working GameCube input with IOHID on that setup.
Other drivers or OS versions may change reports; the parser ignores reports it
does not recognize.

### PS4

The tested PS4 OGCTester 01.02 app used unchanged OGCKit sources matching its
dependency manifest. GameCube button input was user-confirmed through wired
GBros. in XInput mode. This establishes that specific end-to-end OGCKit input
path, not broad PS4 compatibility. Analog/trigger sweeps, hotplug, resource
lifetime, and long-run stability remain unverified by that test. The app is
separate and is not built or tested as part of this repository.

### Adapter-specific limits

GBros. exposes one pad in XInput mode; the manufacturer supports one controller
per adapter. In reported dual-port tests, Wii Extension input took priority over
GameCube input; leave the Wii Extension port empty when evaluating GameCube input.
Reported Nunchuk mappings are Z to A and C to B, with junk frames placing axes in
a corner that consumers should reject. Reported Classic Controller triggers are
digital (0/255). These observations are not claims of full controller coverage.

## PS4 backend and resource risk

The compatibility alias currently frees the device list with `unref=0`. Standard
libusb ownership rules would retain device references and leak them across scans.
Whether those rules apply to `sceUsbd`, and the exact ABI and ownership, remain
unresolved. The public
[OpenOrbis header](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/0a1aaf9dd4a92695538bdeb09fb056d06dd11725/include/orbis/Usbd.h)
declares `sceUsbdFreeDeviceList(list)` with one argument, unlike the existing
two-argument alias, and gives no usable signatures for
`sceUsbdRefDevice`/`sceUsbdUnrefDevice`. A header declaration does not prove runtime
behavior.

The alias and unref policy remain unchanged pending reliable ABI/ownership
evidence and targeted resource-lifetime tests. Working button input in OGCTester
01.02 does not resolve this risk or establish long-run stability.

`sceUsbd` reports libusb error `-N` as `0x80240000 + N`; an idle read is
`0x80240007` (timeout).

### Interface provenance and external dependencies

The local endpoint-descriptor declaration in `src/backend_ps4.cpp` follows the
public libusb 1.0 data layout. It was carried forward from this project's USB
probe and `usbpad` precursor; it contains no library implementation or upstream
Doxygen comments. The `sceUsbdFreeDeviceList` compatibility alias binds a local
declaration to an external symbol; it does not implement that function.

Reference versions used for this comparison:

- [libusb endpoint descriptor](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/libusb.h#L650-L691);
  the header identifies its license as LGPL-2.1-or-later
  ([license text](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/COPYING)).
- [OpenOrbis USB types](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/0a1aaf9dd4a92695538bdeb09fb056d06dd11725/include/orbis/_types/libusb.h)
  and the function header linked above. The type header references, but does not
  define, the endpoint descriptor. OpenOrbis identifies its toolchain repository
  as [GPLv3](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/0a1aaf9dd4a92695538bdeb09fb056d06dd11725/README.md#license).

These are comparison references, not bundled dependencies or a requirement to
use those exact toolchain versions. This source tree contains no SDK headers,
import-stub data, or dependency libraries. OGCKit source code remains under
[MIT](../LICENSE); [branding artwork](../assets/brand/LICENSE.md) is separately
CC BY 4.0. External dependencies retain their own licenses; distributing linked
binaries or SDK materials requires a separate assessment of what is included.
This provenance note is not a legal clearance or a claim that every API
declaration is exempt from copyright.

The compatibility signature and device-reference ownership remain unverified,
as described above. Layout correspondence and source provenance do not establish
runtime ABI correctness or resource safety.

## Hardware-free tests

Parser, report-queue, and fake-backend runtime tests need neither libusb nor a pad:

```sh
cmake -S . -B build-tests -DOGC_BUILD_LIBRARY=OFF
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Coverage includes repeated/concurrent lifecycle calls, init failure, slot limits,
state, disconnect/reconnect, transport errors, open-failure recovery, retry timing
beyond the former unsigned-microsecond rollover, and shutdown with scan/open held
at a synchronization barrier. Waits use test deadlines or CTest's runtime timeout.
Parser tests reject truncated reports whose advertised size exceeds received
data without changing prior state.

For concurrency instrumentation with a supporting compiler, use a separate build
with `-DCMAKE_CXX_FLAGS="-fsanitize=thread -g"` and
`-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"`.

## Recorded build and test scope

- Native hardware-free tests passed on Debian 13.
- The OpenOrbis PS4 cross-build produced a static library containing FreeBSD-targeted
  x86-64 objects. Cross-built test executables were not runnable on Debian; tests
  were separately built and passed with the native compiler.
- The IOHID backend built on macOS, with setup-specific input evidence above.
- A complete libusb library build was not checked in the recorded macOS environment
  because libusb development files were absent. That does not affect the separate
  PS4 cross-build or IOHID build.

These are recorded results, not a claim that every toolchain/platform combination
has been validated.
