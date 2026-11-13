// Shared parsing helpers for Switch-compatible 057E:2009 reports (Bluetooth + USB).
#pragma once

#include "internal.h"

namespace ogc {

inline constexpr uint16_t kSwitchProVendorId = 0x057E;
inline constexpr uint16_t kSwitchProProductId = 0x2009;
inline constexpr uint16_t kSwitchProObservedMaxInputReport = 362;
inline constexpr uint16_t kSwitchProObservedUsbInputReport = 64;
inline constexpr uint8_t kSwitchProHidInterfaceClass = 0x03;
inline constexpr uint8_t kSwitchProFullInputReportId = 0x30;
inline constexpr int kSwitchProBluetoothReportSize = 49;
inline constexpr int kSwitchProUsbReportSize = 64;

inline uint16_t stick12(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | ((data[1] & 0x0F) << 8));
}

inline uint16_t stick12High(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] >> 4) | (data[1] << 4));
}

inline int16_t normalizeStick(uint16_t raw) {
    int32_t value = (static_cast<int32_t>(raw) - 2048) * 16;
    if (value > 32767)
        value = 32767;
    if (value < -32768)
        value = -32768;
    return static_cast<int16_t>(value);
}

inline bool parseFields(const uint8_t* data, ogc_state& state) {
    uint32_t buttons = 0;
    const uint8_t right = data[3];
    const uint8_t shared = data[4];
    const uint8_t left = data[5];

    if (right & 0x01) buttons |= OGC_BTN_Y;
    if (right & 0x02) buttons |= OGC_BTN_X;
    if (right & 0x04) buttons |= OGC_BTN_B;
    if (right & 0x08) buttons |= OGC_BTN_A;
    if (right & 0x40) buttons |= OGC_BTN_RB;
    if (shared & 0x01) buttons |= OGC_BTN_BACK;
    if (shared & 0x02) buttons |= OGC_BTN_START;
    if (shared & 0x04) buttons |= OGC_BTN_R3;
    if (shared & 0x08) buttons |= OGC_BTN_L3;
    if (shared & 0x10) buttons |= OGC_BTN_GUIDE;
    if (shared & 0x20) buttons |= OGC_BTN_CAPTURE;
    if (left & 0x01) buttons |= OGC_BTN_DPAD_DOWN;
    if (left & 0x02) buttons |= OGC_BTN_DPAD_UP;
    if (left & 0x04) buttons |= OGC_BTN_DPAD_RIGHT;
    if (left & 0x08) buttons |= OGC_BTN_DPAD_LEFT;
    if (left & 0x40) buttons |= OGC_BTN_LB;

    state.buttons = buttons;
    state.lt = (left & 0x80) ? 255 : 0;
    state.rt = (right & 0x80) ? 255 : 0;
    state.lx = normalizeStick(stick12(data + 6));
    state.ly = normalizeStick(stick12High(data + 7));
    state.rx = normalizeStick(stick12(data + 9));
    state.ry = normalizeStick(stick12High(data + 10));
    return true;
}

}  // namespace ogc
