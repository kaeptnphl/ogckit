# OGCKit roadmap

The long-term goal is a universal controller-input library for homebrew and
emulators, with broad controller-protocol and connection-transport coverage behind
a common C API. The main-based integration branch includes USB support and narrowly scoped
Bluetooth HID input through macOS IOHID. Today's early proof of
concept is a starting point, not universal support or a complete catalog of
intended protocols and transports. The status below distinguishes the baseline
from this integration branch; upstream review and merge status are separate.
Version `0.1.0` is tentative; there are no promised dates or release commitments.

## Implemented

- Common controller-state C API, device discovery, and synchronized runtime.
- Initial Xbox 360-class XInput report driver.
- Captured Switch-Pro-compatible USB reports from the tested Miadore N64
  receiver in Mode A, with parser and runtime tests.
- Scoped macOS Bluetooth HID input for observed GBros X-Input and
  Switch-compatible report layouts.
- USB backend choices: desktop libusb, macOS IOHID, and PS4 `sceUsbd`.
- Hardware-free parser, runtime, and report-queue tests.
- PS4 OpenOrbis cross-build and limited hardware evidence, including a confirmed
  button-input path through unchanged OGCKit sources in OGCTester 01.02.

Implementation and setup-specific tests do not establish general compatibility.
See the [developer guide](docs/development.md#hardware-evidence) for evidence and scope.

## Main-based integration and upstream review

[PR #1](https://github.com/kaeptnphl/ogckit/pull/1), commit
`7dff7469f89576487512a59618021f7fc8f1b8b4`, is the upstream review source for
the scoped Bluetooth HID paths, passive capture tooling, and captured-report
regressions included in this integration branch. The branch is based on GitHub
`main` and prepared for review; this does not change `main`, and PR #1's review
status remains separate.

- On the tested Mac, GBros Bluetooth X-Input provides analog GameCube triggers.
  The tested S-Input Bluetooth setups provide digital trigger/button fields;
  mode identity cannot be inferred from their shared `057E:2009` VID/PID.
- The RVL-005 Classic Controller was exercised through the Wii Extension port.
  A focused S-Input USB capture confirmed digital L/R-to-LB/RB mapping.
  Analog USB trigger travel and additional report-tail semantics remain unknown.
- Native macOS build and six hardware-free test targets passed. The exact PR
  commit also compiled and linked with OGCTester using the existing PS4
  PacBrew/OpenOrbis toolchain on Emma; ELF/OELF/SELF verification passed.
- The corrected `OGCP10001` test package was installed and started on PS4,
  according to the user. X-Input USB input worked with both controller ports;
  adapter/controller hotplug caused no observed app error in the short test.
  This is not a full mapping or long-run stability validation.
- The original PR snapshot selected `switch-pro-usb` on PS4 but displayed no
  S-Input. A later isolated diagnostic/setup snapshot, `OGCD10003` version
  01.06, is reported by the OGCTester session to send explicitly approved USB
  setup/report-mode commands via Interrupt OUT. Its saved log contains accepted
  64-byte `0x30` reports; the user reported input in all modes/ports they tested.
  This snapshot is **not part of PR #1** and is not read-only. The original PR
  remains passive. Do not repeat output commands without a renewed test scope.
- **Open PS4 work:** the diagnostic log also records reader errors/reconnects.
  Review stability, full mapping/axes, and the isolated setup implementation
  before integrating it into a maintained branch. The user report is not
  exhaustive compatibility or long-run validation.

Work is paused after the 2026-10-24 session. Resume with the saved OGCTester
diagnostic log and source snapshot, not another broad controller test. Keep PR #1
open pending review of the PS4 limitation; no merge has been authorized.
Detailed deployment and package evidence belongs to OGCTester's existing
`BUILD_AUDIT.md`.

## Planned

The following is a rough priority direction for discussion, not a schedule,
delivery promise, or guarantee of compatibility. The order may change as
feasibility and community interest become clearer.

1. **Validate the existing PS4 path.** Continue OGCKit and OGCTester work,
   focusing on currently unverified analog and trigger behavior, hotplug and
   reconnect, and runtime/resource stability.
2. **Expand the controller and transport test matrix.** Investigate the GBros.
   adapter across relevant modes, mappings, and connections—including Switch
   input and Bluetooth—and document which combinations have actually been
   tested. Build broader coverage incrementally across candidate gamepads,
   arcade pads, and retro adapters. Examples include DualShock 3/4, PlayStation
   Move, Wii Remote and extensions, Retro-Bit Saturn, N64 receivers, Logitech
   Dual Action, Switch Pro/Joy-Con, DualSense, Xbox One/Series, and generic HID,
   DirectInput, and XInput devices. This is a test inventory, not a promise to
   support every listed device.
3. **Extend macOS support.** Investigate additional device and integration paths
   using the existing macOS backend.
4. **Explore PS4 emulator integrations.** Investigate OGCKit integration with
   DolphinPS4, Crema, and other emulators after the PS4 path and controller
   behavior are better validated.
5. **Create a small Linux starting point.** Establish a foundation that can
   invite community contributions without making Linux a current project focus.
6. **Assess Android and iOS feasibility.** Investigate platform constraints
   before implying that either platform is supported.

Cross-cutting topics include keeping controller-protocol drivers, connection
transports, platform backends, and consumer integrations distinct; evolving a
normalized input model and discoverable device capabilities; and evaluating API
and ABI stability as integrations grow. Device identity, multiple pads,
hotplug/reconnect, sleep/wake, pairing, permissions, latency, and resource use
also need consideration across platforms and transports.

Rumble is a possible output capability, including separate motors where a device
supports them; it is not implemented. Motion and other sensor input are separate
possible input capabilities. Device profiles and mapping/calibration can support
compatibility; user-configurable remapping may be a later optional direction.
Accessibility should be considered in the input model. Wheels and flight sticks
are outside the current gamepad/arcade/retro-adapter focus.

Quality and adoption work may include repeatable parser/runtime and hardware
tests; a compatibility matrix that distinguishes implemented, tested, and
verified behavior; report capture/replay or virtual devices for diagnostics; and
clear packaging, build, and integration guidance for emulator and homebrew
developers.

Beyond the scoped work in PR #1, broader Bluetooth/controller coverage, rumble,
motion input, and the platform and emulator expansions above remain planned
and are not broadly validated.

## Unknown or unverified

- Compatibility across controller models, adapters, revisions, platforms, and OS
  versions beyond the documented setups.
- PS4 analog/trigger sweeps, hotplug, resource lifetime, and long-run stability
  beyond the documented short user-reported input/hotplug observations.
- PS4 S-Input setup integration and reader stability: input was reported with
  isolated OGCD10003, not with the original passive PR #1 snapshot.
- Exact `sceUsbd` device-list ABI and reference ownership. The unchanged
  `unref=0` compatibility alias risks retaining references under standard libusb
  ownership rules; whether those rules apply to `sceUsbd` remains unresolved.

The [PS4 resource-risk notes](docs/development.md#ps4-backend-and-resource-risk)
describe why a working input path does not resolve the ownership question.
