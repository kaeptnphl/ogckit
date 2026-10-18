# OGCKit — Open Game Controller Kit

Game controller input for console homebrew and emulators. PlayStation first, the core is platform independent.
Not related to libogc.

- Plain C API (`include/ogckit.h`): `ogc_init()`, `ogc_get(slot, &info, &state)`.
- Hotplug by rescanning once per second, one reader thread per pad, thread-safe.
- Read-only: nothing is sent to a pad (no rumble yet).
- **Drivers** (understand a pad): XInput (Xbox 360 class, 8BitDo GBros. in XInput mode). Planned: Switch Pro, DualShock, generic HID.
- **Backends** (reach USB): `ps4` (OpenOrbis/PacBrew, sceUsbd) and `libusb` (Linux/macOS/Windows, for development).
  Other platforms can add a `backend_*.cpp` (see `src/internal.h`). Bluetooth is a goal, not done.

## Build and try (libusb)

    cmake -S . -B build && cmake --build build
    ctest --test-dir build        # parser test, no hardware needed
    build/ogc-terminal            # prints state changes of a connected pad

PS4: `-DOGC_BACKEND=ps4` with the PacBrew toolchain; the PS4 USB code was proven in DolphinPS4.

## Notes from testing (PS4)

- sceUsbd reports libusb error `-N` as `0x80240000 + N`; an idle read is `0x80240007` (timeout).
- The 8BitDo GBros. shows ONE pad in XInput mode, whatever is plugged in (GameCube, Classic, Nunchuk); the
  manufacturer supports one controller per adapter. Nunchuk: Z arrives as A, C as B, plus junk frames with all
  axes in a corner that a consumer should drop. Classic Controller triggers arrive as 0/255 only.
- On the PS4 the device list is freed with `unref=0` (leaks one small reference per device per scan).

## License

MIT, see `LICENSE`.
