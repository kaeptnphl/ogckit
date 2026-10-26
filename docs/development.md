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

IOHID scans device services; USB interface class information comes from
`IOUSBHostInterface`, and protocol drivers decide what matches. The registry
matching query is intentionally not constrained by primary usage: the connected
USB `057E:2009` service was visible through IOHIDManager as a Game Pad but did
not match that usage as an IORegistry service. Bluetooth drivers still require
their observed Game Pad usage and device identity.
Bluetooth matching is restricted to the observed GBros X-Input identity
(`045E:02E0`, max input report size 16) and Switch-Pro-compatible identity
(`057E:2009`, max input report size 362), with Game Pad usage. Their drivers
accept only their observed report layouts and sizes; the matching `057E:2009`
identity does not distinguish physical GBros modes. Devices open
non-exclusively, so macOS and other apps retain input.
Reports arrive on a per-device dispatch queue and enter a bounded queue that keeps
the newest reports. Removal makes reads fail after queued reports drain; close
waits for IOHID's cancel handler before releasing the device.

For read-only Bluetooth HID observation on macOS, build and run
`bluetooth_hid_capture --list` to enumerate visible Bluetooth HID devices and
their product, VID/PID, usage, maximum input size, and raw report descriptor.
The utility does not print serial numbers or Bluetooth addresses. Select a
device only from that live list; `--capture VID PID SECONDS RAW_FILE MARKER_FILE`
requires an exact VID/PID match and exactly one matching Bluetooth device, and
caps capture time at 300 seconds. Both output paths are created exclusively
(existing files are not overwritten). It sends no output reports and performs
no device initialization.

The utility also accepts `--list-usb` and
`--capture-usb VID PID SECONDS RAW_FILE MARKER_FILE` for USB HID devices exposed
through macOS IOHID, with the same exact-identity, bounded, exclusive-file and
read-only behavior. This observes only system-visible USB HID devices; it does
not capture arbitrary USB interfaces or replace the library's libusb
interrupt-endpoint path. No USB S-Input device is claimed supported until its
reports have been observed; the tested Miadore receiver in Mode A is the scoped
exception described below.

After IOHID opens the selected device, wait for `FIRST_REPORT_RECEIVED` before
starting input. Enter these commands in order to record boundaries in the
separate marker file, each tied to the last fully written report number and
cumulative report-byte offset: `1-start`, `1-end`, `2-start`, `2-end`, `3-start`,
`3-end`. The three blocks are buttons (A/B/X/Y/Z/Start/Home/Share), D-pad and
both sticks, then L/R analog and end-clicks. Raw callback report IDs and bytes
are written unchanged to the raw file. This captures evidence only; it does not
interpret S-Input reports or issue protocol commands.

The read timeout bounds waiting, not report delivery latency. Reports are
delivered as they arrive. Open failures are logged with the IOReturn code.

## Hardware evidence

The GBros. adapter is a versatile initial test platform, not the library's scope.
The following observations establish only the paths and setups described.

### macOS

On one setup, Apple's `XboxGamepad` driver exposed a USB GBros. (`045E:028E`) as a
20-byte report without a report ID, retaining the raw XInput byte layout. A short
normal-user IOHID run received about 118 reports per second and shut down cleanly
twice. OGCTester later reported working GameCube input with IOHID on that setup.

The Miadore 2.4 GHz N64 receiver was passively captured in two user-selected
modes. Its XInput-style mode exposed `045E:028E`, report ID 0, 20-byte reports
beginning `00 14`, compatible with the existing XInput parser. In Mode A
(called D-Input by the user), macOS exposed `057E:2009` as a generic
`Pro Controller` USB HID interface (`03/00/00`), with report ID `0x30` and
64-byte reports. The reports use packed 12-bit axes: the N64 stick drives the
first pair and the four C directions drive the second pair. Captured button
reports mapped A, B, Start, Select, Home, Z, L, R, and ZR to distinct fields;
D-pad directions are button bits. L/R were digital in the observed sequence.
These are reports from one receiver and firmware, not certification of other
revisions, modes, or platforms. The descriptor advertises output report ID
`0x31`; neither the capture tool nor parser sends or implements output reports.
The parser is covered by hardware-free tests. Live macOS IOHID discovery and
input were verified in Mode A, including button changes, both stick pairs, and
digital Z/ZR trigger states.

On the same Mac, an OS-paired GBros in X-Input mode appeared as Bluetooth
`IOHIDUserDevice` (`045E:02E0`), Generic Desktop/Game Pad usage, with a 16-byte
maximum input report and descriptor report IDs 1–4. The capture utility recorded
callback report ID separately from report bytes; the observed gamepad input was
report ID 1 with a 16-byte payload beginning with `01` (the report ID is included
in the callback buffer). The descriptor evidence below came directly from the
live service's `ReportDescriptor` CFData property through
`IOHIDDeviceGetProperty` in `tools/bluetooth_hid_capture.cpp`; the captured descriptor
was 306 bytes. Its parsed items identify report ID 1 as a Generic Desktop/Game
Pad collection with X/Y and Rx/Ry 16-bit axes, Z/Rz 10-bit analog fields, a
four-bit Hat Switch (logical values 1–8, null state enabled), Button usages 1–10,
and padding. Report ID 2 is a Generic Desktop System Control collection
containing System Main Menu usage 0x85 as a one-bit input. Report ID 3 is an
output report in the Physical Interface Device page. Report ID 4 is an eight-bit
input in the Generic Device Controls page (usage 0x20). This descriptor, rather
than a generic layout assumption, informed the report parser:

- A/B/X/Y, Start, Z, and four D-pad directions changed the same report's button
  fields consistently. A/B/X/Y were payload byte 14 masks `01/02/04/08`; Start
  was `80`, and Z was `40`, mapped to the existing right-shoulder
  `OGC_BTN_RB` semantic used by the wired XInput mapping. The D-pad values in
  payload byte 13 were up/right/down/left = `01/03/05/07`; the descriptor
  declares the full 1–8 Hat Switch range, so the parser expands codes 2/4/6/8 to
  diagonals, treats 0 as neutral (observed idle/null state), and rejects values
  outside 0–8.
- Main-stick axes used payload bytes 1–4 and C-stick axes bytes 5–8 as little-endian
  16-bit values, centered at `0x8000`. Main-stick directional sweeps supported
  right-positive X and up-positive Y in `ogc_state`. The C-stick's resting X
  value drifted on this controller; raw values are preserved without a deadzone
  or calibration.
- L/R analog trigger values used little-endian 10-bit fields at payload bytes
  9–10 and 11–12. End-click reports set payload byte 14 masks `10` and `20`.
- Z and Start produced distinct payload byte 14 values `40` and `80`; Start maps
  to `OGC_BTN_START`, and Z maps to `OGC_BTN_RB`. A combined Home/Share action
  produced one report-ID-2 press/release pair (`02 01`, `02 00`). The descriptor
  identifies ID 2 as System Main Menu, so that report updates `OGC_BTN_GUIDE`,
  preserving the bit across intervening ID-1 gamepad reports and clearing it on
  ID-2 release. The combined action does not reveal whether Share independently
  emits a report; although `ogc_state` has `OGC_BTN_BACK`, no captured evidence
  justifies assigning Share to it.

The GBros Bluetooth parser is matched only for Bluetooth transport plus VID/PID
`045E:02E0`; it does not claim generic Bluetooth XInput support. Observed
report ID 1/16-byte gamepad payloads and descriptor-identified report ID 2
System Main Menu press/release are parsed. Report IDs 3–4 and unobserved button
usages are ignored. This is hardware evidence for one adapter, mode, OS, and
controller—not proof of all GBros behavior, Share-button mapping, or complete
control coverage. Other drivers or OS versions may differ; unrelated reports are
ignored.

### Classic Controller RVL-005, one continuous S-Input run

A Classic Controller RVL-005 was connected to the GBros Wii Extension port and
observed over Bluetooth in the user-designated S-Input/GameCube setup. The one
continuous capture contains 7,886 reports, all callback report ID `0x30` and
49-byte payloads. The user performed the requested sequence before the separate
marker commands were entered, so the later marker ranges do not delimit the
button, D-pad, stick, and shoulder actions and must not be interpreted as
omitted test steps.

The whole-run chronology has distinct standard-field activity: A/B/X/Y at
reports 4557/4608/4664/4720; Minus/Plus/Classic Home at 4815/4889/4967;
digital ZL/ZR fields at 5168/5210; D-pad byte-5 masks `02/04/01/08` at
5496/5557/5609/5653; stick-axis activity later in the same run; and L/R
shoulder bits at 6664/6893. The parser yields A/B/X/Y, Back/Start/Guide,
binary `lt`/`rt`, and LB/RB respectively. The D-pad samples parse according
to standard Switch report semantics (`02` up, `04` right, `01` down, `08`
left). This guards parser-field decoding, not an independent physical-direction
validation: action-level markers were absent, and chronology alone is not used
to infer a custom physical remapping.

Captured stick fields varied substantially, including near-range extrema and
noisy excursions while both sticks were exercised. A captured sample guards
standard decoding only; it does not establish clean directional endpoints or
calibration values. No analog trigger intermediate values were identified in
this run; L/R appeared as digital shoulder bits, and ZL/ZR as digital trigger
bits. These are observations for this RVL-005, adapter, mode, and tested
firmware (version not recorded), not an inference about every Classic
Controller or GBros firmware.

### Switch-Pro-compatible Bluetooth report evidence

In a separate user-designated GameCube-mode session, macOS exposed one Bluetooth
HID device as `Pro Controller`, VID/PID `057E:2009`, Generic Desktop/Game Pad,
with a 170-byte descriptor and `MaxInputReportSize` 362. The descriptor listed
input report IDs `21`, `30`–`33`, and `3F` (and output IDs `01`, `10`–`12`).
The passive capture observed only input report ID `30`, 49 bytes, in all three
marked test blocks. A later user-switched Switch-Pro-mode capture on the same
Mac reported the identical HID identity, descriptor, and report ID/length.
Thus `057E:2009` does not distinguish the two adapter modes; the product string
and VID/PID do not prove physical operating mode or make this a
GameCube-exclusive driver.

The `0x30` fields follow the publicly documented Nintendo Switch controller
full-input layout in the Linux kernel
[`hid-nintendo.c`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c)
and the
[Nintendo Switch Bluetooth HID notes](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/bluetooth_hid_notes.md).
Those references describe the report ID/timer, button bytes, packed 12-bit
sticks, and standard `0x30` full-input report. Our capture confirms the offsets:
the observed A/B/X/Y values in byte 3 were `08/04/02/01`; the D-pad in byte 5
produced `02/04/01/08` for up/right/down/left; and the left/right stick fields
varied at bytes 6–8 and 9–11. The tested trigger sequence produced bit `80` in
byte 5 for L and byte 3 for R. The parser is deliberately limited to Bluetooth
`057E:2009` and exact 49-byte input report `0x30`; USB has a separate exact
64-byte match described below. Neither driver claims generic Bluetooth/GameCube
mode detection.

Button meanings follow the referenced standard report fields, not an inferred
custom adapter protocol: byte 3 contains Y/X/B/A, R, and ZR; byte 4 contains
Minus, Plus, stick clicks, Home, and Capture; byte 5 contains the D-pad, L, and
ZL. Plus maps to `OGC_BTN_START`, Home to `OGC_BTN_GUIDE`, and Minus to
`OGC_BTN_BACK`; Capture (`byte 4`, bit `0x20`) maps to the additive
`OGC_BTN_CAPTURE` button bit (bit 15), distinct from `OGC_BTN_BACK`; standard
R/L map to `OGC_BTN_RB`/`OGC_BTN_LB`, and standard ZR/ZL map to binary
`rt`/`lt` values of 255 or 0. `ogc_state` layout is unchanged.

Two passive, bounded captures of user-designated adapter modes confirm that
the same reports from the same `057E:2009` HID identity have different
physical-button mappings. The table records observed report fields and the
existing standard parser output; it does not add mode detection or claim
universal mappings.

| Physical setup as designated during capture | Physical control | Observed report field | Standard parser output |
|---|---|---|---|
| GameCube mode | Z | byte 3 `0x40` (R) | `OGC_BTN_RB` |
| GameCube mode | L / R | byte 5 `0x80` (ZL) / byte 3 `0x80` (ZR) | `lt=255` / `rt=255` |
| GameCube mode | Share | byte 4 `0x20` (Capture) | `OGC_BTN_CAPTURE` |
| Switch-Pro mode | Z | byte 3 `0x80` (ZR) | `rt=255` |
| Switch-Pro mode | L / R | byte 5 `0x40` (L) / byte 3 `0x40` (R) | `OGC_BTN_LB` / `OGC_BTN_RB` |
| Switch-Pro mode | Share | byte 4 `0x20` (Capture) | `OGC_BTN_CAPTURE` |

The GameCube-mode Z/Share retry capture is
`gc-mode-z-share-capture-retry.txt`; its marked block spans reports 259–2833
(byte offsets 12,691–138,817). It observed no `BACK` field in that Z/Share
sequence. The later Switch-Pro-mode capture and markers are
`switch-pro-mode-capture-20261023.txt` and
`switch-pro-mode-markers-20261023.txt`; all three marked blocks observed only
ID `0x30`/49-byte reports. The captures show standard report-field differences,
not separate driver identities. The implementation deliberately preserves
standard Switch report semantics and does not infer or equate adapter modes
from the shared VID/PID.

**Practical trigger result for the tested GBros/GameCube-controller setup:**
neither of the two user-designated Switch modes exposed analog trigger travel
over Bluetooth. In GameCube mode, L/R changed the standard digital ZL/ZR bits;
in Switch-Pro mode, L/R changed the standard digital L/R shoulder bits. During
the Switch-Pro-mode block, the user moved each trigger through partial travel,
to its end-click, and released it, but the `0x30` reports carried only those
digital button bits—no analog intermediate value was available to OGCKit. The
descriptor-declared input report ID `0x3F` lists buttons, a hat, and four stick
axes, but it was not observed in these captures and does not establish trigger
coverage for this setup. By contrast, the separate GBros X-Input Bluetooth
capture contained analog L/R trigger values in its ID-1 reports. For analog
GameCube trigger input on this tested adapter/controller setup, use GBros
X-Input mode.

This is a direct hardware result for the tested GBros, attached GameCube
controller, Bluetooth connection, and observed report behavior; adapter
firmware version was not recorded, so it is not a claim about every firmware,
adapter, or Switch controller. No automatic mode detection, per-mode override,
or inferred analog trigger values are implemented. Stick values are decoded as
unsigned 12-bit pairs and currently centered at the protocol range midpoint
2048 into `ogc_state`'s signed range; no rest-state drift is learned.

### User-designated GBros S-Input USB capture

After the user connected the adapter by USB and disconnected Bluetooth,
macOS IOHID enumerated a `Pro Controller`, USB `057E:2009`, Generic Desktop/Game
Pad, with a 203-byte report descriptor and 64-byte maximum input reports. The
product string and identity describe what macOS exposed; they do not independently
prove the physical adapter mode. A bounded passive run captured 8,002 reports,
all callback report ID decimal 48 (`0x30`) and 64 bytes. The recorded markers
bracket reports 682–7408; the utility's fixed `buttons` marker label is not an
action-level partition because the user performed the entire compact sequence
inside that one interval. It sent no output reports or initialization commands.

The first 13 report bytes contain standard `0x30`-compatible fields and the
captured samples exercised them: A/B/X/Y in byte 3 (`08/04/02/01`), Minus/Plus/
Home in byte 4 (`01/02/10`), ZL/ZR digital bits in byte 5/byte 3 (`80`), D-pad
byte-5 masks `02/04/01/08`, and changing packed stick fields in bytes 6–11.
The added USB driver matches only USB `057E:2009`, a HID interface with 64-byte
input size, and report `0x30` of exactly 64 bytes; it applies the observed
standard fields and ignores the remaining payload.

The initial full-control run included L/R actions but showed no standard L/R
shoulder bits (`byte 5`/`byte 3` mask `0x40`), so it did not establish their
mapping. A subsequent focused, passive L/R-only capture collected 3,844 complete
64-byte reports. In the order of the requested L, R, repeat-L, repeat-R sequence,
the standard fields changed in byte 5 (`0x40`) at reports 1950–2032 and
3161–3211, then byte 3 (`0x40`) at 2662–2927 and 3258–3304. These captured
reports establish digital L-to-`OGC_BTN_LB` and R-to-`OGC_BTN_RB` for this
tested USB setup; they do not establish analog trigger travel. No `0x80` ZL/ZR
bits occurred in this focused run.

The remaining 51 report bytes were unchanged throughout each L/R-active span,
but did vary elsewhere in the capture (including before and after the prompted
shoulder sequence). Their semantics remain unknown; no tail mapping or analog
claim is inferred from those unrelated changes. The parser regressions use
complete captured 64-byte L/R reports and verify the standard output fields.
Stick values in the broader run were noisy and are not calibration evidence;
broader device/mode coverage remains unverified.

After the adapter was reconnected, a bounded hardware smoke test of the actual
macOS IOHID backend completed successfully: `ogc_init()` returned 0, the core
logged slot 0 as `057E:2009 (switch-pro-usb)`, and repeated `ogc_get()` calls
received 546 input reports during the 8-second observation, with final
`buttons=0`, `lt=0`, `rt=0` and centered-range axes. `ogc_shutdown()` completed
cleanly. This validated discovery, opening, ID/length parsing, live report
delivery and shutdown without sending output reports or asking for another
control sequence. The query fix above was required because constraining the
IORegistry match itself to primary Game Pad usage omitted this USB service.

### Switch Bluetooth SPI calibration experiment (not enabled)

This historical read-only experiment concerned the Bluetooth `0x21` subcommand
reply format; it was separate from the USB `0x30` captures above.

The Linux kernel `hid-nintendo.c` documents calibration reads using SPI flash
subcommand `0x10`, user-calibration magic at `0x8010`/`0x801B`, and factory
stick blocks at `0x603D`/`0x6046` (9 bytes each). Its `joycon_input_report`
layout includes the report ID at byte 0; the `joycon_subcmd_reply` union then
places ACK at byte 13, subcommand ID at byte 14, the four-byte little-endian
SPI address at bytes 15–18, size at byte 19, and flash data from byte 20. This
follows the kernel's packed input-report and subcommand-reply structures and
`joycon_request_spi_flash_read()` reading `subcmd_reply.data[5]`.

An earlier bounded experiment sent four read requests, but its response parser
used incorrect ID-`0x21` offsets (subcommand 10, ACK 11, address 12–14, size
15, data 16). Consequently, its timeout logs mean only that no reply was
accepted by that parser; they do not establish whether the device replied.
No further hardware attempt was made. To preserve OGCKit's read-only contract,
the calibration output path is now removed and normal `ogc_init()` performs no
calibration queries. Sticks continue using raw midpoint normalization; drift is
not learned. A future calibration feature would require an explicit opt-in and
tests against the source-confirmed report layout before any hardware use.

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
