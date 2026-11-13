# DualShock 4 Integration — Plan & Erkenntnisse

Dieser Branch (`ds4-cline`) sammelt die Planung und die ersten Erkenntnisse für die Integration des Sony DualShock 4 (DS4) in OGCKit.

## Ausgangslage

- **Projekt:** OGCKit (Open Game Controller Kit), Branch `main`
- **Plattform:** macOS (Apple M1 / M-Serie)
- **Ziel:** DS4 als neuen Controller-Treiber in OGCKit einbinden
- **Status:** Planung / Recherche, noch kein Code geschrieben

## Architektur-Entscheidung

OGCKit trennt strikt zwischen **Protokoll-Treibern** (`src/drv_*.cpp`) und **Plattform-Backends** (`src/backend_*.cpp`). Der DS4-Driver wird ein neuer Treiber nach dem Muster der bestehenden Driver:

- `src/drv_xinput.cpp` (56 Zeilen, Template)
- `src/drv_switch_pro_bluetooth.cpp` (101 Zeilen)
- `src/drv_gbros_bluetooth.cpp` (100 Zeilen)

Der Driver implementiert das `Driver`-Interface aus `src/internal.h`:

```cpp
struct Driver {
    const char* name;
    const InterfaceInfo* (*match)(const DeviceInfo& dev);
    bool (*parse)(const uint8_t* data, int len, ogc_state& state);
};
```

- `match()` erkennt den DS4 über `vid == 0x054C` und bekannte PIDs.
- `parse()` übersetzt die rohen HID-Reports in `ogc_state` (Buttons als Bitmask, Sticks als `int16_t`, Trigger als `uint8_t`).

OGCKit ist **read-only** — es werden keine Rumble-, Lightbar- oder Feature-Reports gesendet.

## Backend-Wahl auf macOS

| Backend | Voraussetzungen | Empfehlung |
|---|---|---|
| `iohid` | IOKit + CoreFoundation (systemeigen) | ✅ Erster Start, kein Homebrew nötig |
| `libusb` | pkg-config + libusb 1.0 dev files | Später, falls benötigt |

Aktuell fehlen auf dem Entwicklungsrechner noch `cmake` und die `libusb`-Entwicklungsdateien. Deshalb wird mit `iohid` gestartet:

```sh
brew install cmake
cmake -S . -B build-iohid -DOGC_BACKEND=iohid
cmake --build build-iohid
```

## Vergleich mit SDL3 Gamepad

Siehe Gesprächsverlauf: OGCKit vs. SDL3 (https://github.com/libsdl-org/SDL, https://wiki.libsdl.org/SDL3/CategoryGamepad).

**Wesentliche Unterschiede:**

| Aspekt | OGCKit | SDL3 Gamepad |
|---|---|---|
| Mapping-Ansatz | Eigener Driver pro Controller | Zentrale `gamecontrollerdb.txt` |
| Rumble / Lightbar | ❌ Nicht implementiert | ✅ Unterstützt |
| Ereignisse / Callbacks | ❌ Nur polling | ✅ SDL_Event |
| Lizenz | MIT | zlib (kompatibel) |

### Kann die SDL-Datenbank verwendet werden?

**Ja, aber nur als Referenzquelle, nicht als direkter Code.**

- Die SDL-Datenbank (`gamecontrollerdb.txt`) nutzt ein SDL-eigenes Textformat. OGCKit hat keinen Parser dafür und das würde zum aktuellen Driver-Design nicht passen.
- Die Datenbank ist nützlich, um **VID/PID** und die **Button/Achsen-Belegung** des DS4 nachzuschlagen.
- Lizenztechnisch ist die Übernahme von Informationen (VID/PID, Belegung) unproblematisch (zlib + MIT sind kompatibel), solange Urheberrechtshinweise beibehalten werden.

DS4-IDs (aus SDL-Datenbank als Referenz):
- USB: `054C:05C4` (DS4), `054C:09CC` (Slim), `054C:0CE6` (v2)
- Bluetooth: `054C:05C4` (andere Report-Struktur als USB)

## Nächste Schritte

1. **Build zum Laufen bringen:** `iohid`-Backend auf macOS kompilieren und `ogc-terminal` starten.
2. **DS4-Reports erfassen:** Echten DS4 anschließen und HID-Reports mitschneiden (z. B. mit einem kleinen IOHID-Tool, basierend auf `tools/bluetooth_hid_capture.cpp` als Vorlage).
3. **Driver schreiben:** `src/drv_dualshock4.cpp` mit `match()` und `parse()` erstellen.
4. **Unit-Tests:** `tests/test_dualshock4.cpp` mit den erfassten Reports (hardwarefrei, kein Controller nötig).
5. **Einbinden:** Driver in `src/core.cpp` (`kDrivers`-Array) und `CMakeLists.txt` registrieren.
6. **End-to-End:** Mit echtem DS4 testen, ob Buttons, Sticks und Trigger korrekt in `ogc_get()` ankommen.

## Offene Fragen

- Welche Report-Größe und Struktur liefert der DS4 über IOHID auf macOS (USB vs. Bluetooth)?
- Soll der DS4-Driver USB und Bluetooth in einem Driver abdecken oder zwei getrennte Driver verwenden?
- Wird später Motion (Accel/Gyro) oder Touchpad unterstützt?
