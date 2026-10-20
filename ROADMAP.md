# OGCKit roadmap

The long-term goal is a universal controller-input library for homebrew and
emulators, with broad controller-protocol and connection-transport coverage behind
a common C API. That direction may include Bluetooth and additional transports;
the current implementation connects over USB only. Today's early proof of
concept is a starting point, not universal support or a complete catalog of
intended protocols and transports. Version `0.1.0` is tentative; there are no
promised dates or release commitments.

## Implemented

- Common controller-state C API, device discovery, and synchronized runtime.
- Initial Xbox 360-class XInput report driver.
- USB backend choices: desktop libusb, macOS IOHID, and PS4 `sceUsbd`.
- Hardware-free parser, runtime, and report-queue tests.
- PS4 OpenOrbis cross-build and limited hardware evidence, including a confirmed
  button-input path through unchanged OGCKit sources in OGCTester 01.02.

Implementation and setup-specific tests do not establish general compatibility.
See the [developer guide](docs/development.md#hardware-evidence) for evidence and scope.

## Planned

- Expand controller-protocol coverage incrementally; Switch Pro, DualShock, and
  generic HID are examples of intended driver families, not an exhaustive catalog
  or commitment to a specific delivery order.
- Broaden platform and connection-transport coverage through additional backends
  and transport integrations as development progresses.
- Explore Bluetooth as a future connection transport.

These are long-term directions, not guarantees about supported devices or delivery
order. Bluetooth and rumble are not implemented; no rumble delivery commitment is
made.

## Unknown or unverified

- Compatibility across controller models, adapters, revisions, platforms, and OS
  versions beyond the documented setups.
- PS4 analog/trigger sweeps, hotplug, resource lifetime, and long-run stability
  beyond the confirmed button-input test.
- Exact `sceUsbd` device-list ABI and reference ownership. The unchanged
  `unref=0` compatibility alias risks retaining references under standard libusb
  ownership rules; whether those rules apply to `sceUsbd` remains unresolved.

The [PS4 resource-risk notes](docs/development.md#ps4-backend-and-resource-risk)
describe why a working input path does not resolve the ownership question.
