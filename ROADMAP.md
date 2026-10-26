# OGCKit roadmap

OGCKit is an early proof of concept. Its `0.1.0` version is tentative; this roadmap
describes current evidence and direction, not release commitments or dates.

## Implemented

- Platform-independent C API and synchronized process-wide runtime for controller state.
- XInput-format driver, including the 8BitDo GBros. in XInput mode.
- USB backend choices at configure time: libusb, macOS IOHID, and PS4 `sceUsbd`.
- Hardware-free parser, runtime, and report-queue tests.
- Successful PS4 cross-build with the OpenOrbis toolchain.

These entries describe code or build/test coverage, not broad guarantees for every
controller, operating system, toolchain, or target.

## Planned

- Add drivers for Switch Pro, DualShock, and generic HID controllers.
- Explore Bluetooth support.

Plans have no committed schedule and may change as implementation and device testing
progress.

## Unknown or unverified

- OGCKit's full PS4 runtime compatibility. A separate PS4 application has been run with
  a wired GBros. adapter in XInput mode, but that does not validate OGCKit or its PS4
  backend at runtime.
- The exact `sceUsbd` device-list ABI and reference-ownership behavior. The current PS4
  compatibility alias frees the list with `unref=0`; under standard libusb ownership
  rules this retains device references across scans. Whether that rule applies to
  `sceUsbd` remains unresolved, so the alias and unref policy are intentionally unchanged.
- The complete set of supported controllers and behavior across device revisions,
  operating-system versions, and hardware configurations. The documented GBros. and
  macOS/PS4 observations are setup-specific, not general compatibility guarantees.
- Bluetooth operation, which is a goal but is not implemented.

For setup details and supporting evidence, see the
[README](README.md#hardware-observations) and its [PS4 notes](README.md#ps4-notes).
