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

The Miadore 2.4 GHz N64 receiver was passively captured in two user-selected
modes. Its XInput-style mode exposed `045E:028E`, report ID 0, 20-byte reports
beginning `00 14`, compatible with the existing XInput parser. In Mode A (called
D-Input by the user), macOS exposed `057E:2009` as a generic `Pro Controller`
USB HID interface (`03/00/00`), with report ID `0x30` and 64-byte reports.
Those reports use packed 12-bit axes: the N64 stick drives the first pair and
the four C directions drive the second pair. Captured button reports mapped A,
B, Start, Select, Home, Z, L, R, and ZR to distinct fields; D-pad directions
are button bits. L/R were digital in the observed sequence. These are reports
from one receiver and firmware in two modes, not certification of other
receiver revisions, modes, or platforms. The descriptor also advertises output
report ID `0x31`; the capture and parser do not send or implement output reports.
The captured parser path is covered by hardware-free tests. Live macOS IOHID
discovery was verified with the receiver in Mode A (`057E:2009`,
`switch-pro-usb`). A live test with the Miadore receiver confirmed button
changes, both stick axes (including the C-direction pair), and digital Z/ZR
trigger states. This is evidence for the tested receiver and setup, not a
compatibility guarantee for other revisions or platforms.

In the same receiver's XInput-style mode, macOS exposes `045E:028E` through
its `Xbox360Gamepad` HID driver and OGCKit matches it as `xinput`. In a live
smoke test, button state changes were observed and decoded (including A/B and
D-pad directions), along with left-stick movement reaching both axes. The
device was repeatedly dropped and rediscovered after IOHID read errors (`-1`);
closing also logged `0xE00002C2`. Right-stick and trigger changes have not yet
been verified in this mode. Investigate the macOS IOHID/XboxGamepad interaction
and reconnect behavior before claiming stable XInput-mode runtime support
through this backend. The intermittent loss did not recur during the later
Mode A test; the user also reports that Mode A does not show this behavior.
The cause remains undetermined and should not be attributed to the controller's
idle timer based on current evidence.

A bounded diagnostic run traced this to IOHID removal callbacks while the
receiver remained plugged in and the user was actively moving the stick and
pressing buttons. Each removal callback returned success (`result=0`) after
thousands of accepted reports, with the last report about 0.65 seconds earlier.
Across removals, all three registry identities changed: the HID service, its
USB interface, and the parent USB device. For example, one XInput handle
(`hid=4295169981`, `usb-interface=4295169978`, `usb-device=4295169967`) was
removed, then a newly identified handle opened and began receiving reports.
This is evidence that macOS recreated the USB/HID registry services, not just
that OGCKit's parser rejected a report. It does not by itself prove an
electrical disconnect or identify whether the receiver, USB stack, or a
driver-triggered reset caused the recreation. No input-report error callback
preceded the removals. Closing the removed IOHID handle continued to return
`0xE00002C2` (`kIOReturnBadArgument`), which is secondary to the removal.

Local macOS Unified Logs add a transport-level error for the same
`045E:028E` device (`SWITCH CO.,LTD.`, `Usb Controller`): IOUSBHostFamily
repeatedly reported endpoint `0x81` failing with `0xe00002e8` ("babble
error"), with zero bytes transferred. The local IOKit SDK defines this status
as `kIOReturnOverrun` ("data overrun"; packet too large or more data than the
buffer). After repeated failures, the kernel logged `reset API call`, destroyed
the USB device service, and enumerated the receiver again at 12 Mbps. This
reset/re-enumeration cycle continued after the OGCKit test process had stopped.
Thus the immediate failure is below OGCKit's report parser and is not caused
solely by its reader lifecycle. The evidence still cannot distinguish a
receiver/firmware USB-transfer problem from a macOS Xbox USB driver, USB
controller, or connection-path interaction.

The first follow-up moved the receiver to a different direct Mac USB port and
left it connected for at least 60 seconds with OGCKit closed. Because the Mac
has only two ports, the user also unplugged the Mac power adapter to make the
port available. Thus both the receiver's USB location and the Mac's AC power
state changed; this was not an isolated port comparison. The old location
(`0x01100000`) logged "hardware connection lost" during the unplug; the
receiver then enumerated at the new location (`0x00100000`) at 14:24:18 local
time. No endpoint `0x81` babble/transaction errors or kernel reset/re-enumeration
were observed in the following 72-second window.

A later comparison kept the Mac on battery and OGCKit closed for both ports.
On the original location (`0x01100000`), no babble, transaction, or reset
events appeared during the 60-second observation window after enumeration.
The receiver was then moved to the other direct port, where it enumerated at
`0x00100000`; again, no such errors appeared during the subsequent 60-second
window. The comparison controls for AC-vs-battery state, but each observation
is short and the receiver was physically reconnected between tests. It does
not establish that either port is more reliable or rule out intermittent
receiver/firmware or USB-stack behavior.

A verified 60-second OGCKit run was then performed with the Mac on battery,
the receiver left on the alternate port (`LocationID 0x00100000`), and Mode B
active. Before the timed input period, the visible terminal confirmed a
successful IOHID open as `045E:028E (xinput)` and a 20-byte input report. During
the test, the user repeatedly pressed A and moved the left stick; the terminal
showed changing button states and left-stick values across both axes, including
return to neutral. No OGCKit removal/read-error messages appeared, and the
kernel logs for the interval contained no USB babble, transaction, or reset
events. This confirms stable XInput input for this one run on the alternate
port and battery power, but does not establish long-term reliability or
whether the earlier failures were due to the original port, AC power state, or
intermittent receiver/USB-stack behavior. Right-stick and trigger behavior
were not covered by this run.

The same verified 60-second OGCKit input test was repeated on the original
port (`LocationID 0x01100000`), keeping the Mac on battery and Mode B active.
The visible terminal showed A-button presses and changing left-stick values
during the timed interval. No OGCKit removal/read-error messages appeared, and
the corresponding kernel log interval contained no USB babble, transaction, or
reset events. Together, these two short tests did not reproduce the earlier
XInput disconnect on either port under battery power. They do not establish
long-term stability or identify the cause of the earlier transport errors.

In subsequent XInput mapping probes on the original port, isolated presses of
the N64 C-direction buttons changed button bits while `R(x,y)` stayed at zero:
C-left=`0x0008`, C-right=`0x0200`, C-up=`0x0400`, C-down=`0x0004`. These
controls are correctly exposed as digital buttons for this N64 controller;
`R(x,y)` remaining zero is expected because this controller has no second
analog stick. Isolated trigger presses identified Z as `LT=255` and ZR as
`RT=255`; releasing either returned its value to zero. These are digital
trigger states in the observed XInput reports; analog travel was not tested.

During these same XInput mapping probes, IOHID removal/read errors recurred,
and the kernel logged endpoint `0x81` babble errors followed by USB
reset/re-enumeration on the original USB port. Multiple new HID/USB service
IDs were opened after removals. Thus the two previous one-minute runs were
valid but short samples that did not reproduce the issue, not evidence that
the controller is stable; later runs again reproduced the transport failure.
The timing does not prove that any particular button press caused the USB
error.

The same diagnostic also observed the expected service recreation when the
user intentionally switched from Mode A to XInput: the old Mode-A services
were removed, then new XInput services appeared. This intentional transition
is distinct from the repeated removals during the later stable-mode XInput
test.

The user later tried the Start+A chord, which is documented by the user as a
Mode C switch intended for Nintendo Switch, and observed the controller LEDs
return to Mode A after a few seconds. No Switch is available in this test
setup, so this does not establish whether Mode C works on Switch. The user
reports the manufacturer specifies only Modes A and B for Mac; treat Mode C
behavior on macOS as out of scope and do not conflate the LED observation with
the XInput-mode IOHID reader errors above.

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
[OpenOrbis header](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/Usbd.h)
declares `sceUsbdFreeDeviceList(list)` with one argument, unlike the existing
two-argument alias, and gives no usable signatures for
`sceUsbdRefDevice`/`sceUsbdUnrefDevice`. A header declaration does not prove runtime
behavior.

The alias and unref policy remain unchanged pending reliable ABI/ownership
evidence and targeted resource-lifetime tests. Working button input in OGCTester
01.02 does not resolve this risk or establish long-run stability.

`sceUsbd` reports libusb error `-N` as `0x80240000 + N`; an idle read is
`0x80240007` (timeout).

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
at a synchronization barrier. Captured XInput and Switch Pro USB parser reports
are tested without hardware. Waits use test deadlines or CTest's runtime timeout.
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
