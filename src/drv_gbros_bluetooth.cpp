// Bluetooth X-Input reports observed from the 8BitDo GBros adapter (045E:02E0).
#include "internal.h"

namespace ogc {
namespace {

constexpr uint16_t kGbrosVendorId = 0x045E;
constexpr uint16_t kGbrosBluetoothProductId = 0x02E0;
constexpr int kInputReportId = 0x01;
constexpr int kInputReportSize = 16;

int16_t axis(uint8_t low, uint8_t high, bool invert) {
    const int32_t raw = static_cast<uint16_t>(low | (static_cast<uint16_t>(high) << 8));
    int32_t value = raw - 32768;
    if (invert)
        value = -value;
    if (value > 32767)
        value = 32767;
    if (value < -32768)
        value = -32768;
    return static_cast<int16_t>(value);
}

uint8_t trigger(const uint8_t* report, int lowOffset, int highOffset, uint8_t clickMask) {
    const uint16_t raw = static_cast<uint16_t>(report[lowOffset]) |
                         (static_cast<uint16_t>(report[highOffset] & 0x03) << 8);
    const uint8_t analog = static_cast<uint8_t>((raw * 255u + 511u) / 1023u);
    return report[14] & clickMask ? 255 : analog;
}

bool dpad(uint8_t value, uint32_t& buttons) {
    switch (value) {
        case 0x00: buttons = 0; return true;
        case 0x01: buttons = OGC_BTN_DPAD_UP; return true;
        case 0x02: buttons = OGC_BTN_DPAD_UP | OGC_BTN_DPAD_RIGHT; return true;
        case 0x03: buttons = OGC_BTN_DPAD_RIGHT; return true;
        case 0x04: buttons = OGC_BTN_DPAD_DOWN | OGC_BTN_DPAD_RIGHT; return true;
        case 0x05: buttons = OGC_BTN_DPAD_DOWN; return true;
        case 0x06: buttons = OGC_BTN_DPAD_DOWN | OGC_BTN_DPAD_LEFT; return true;
        case 0x07: buttons = OGC_BTN_DPAD_LEFT; return true;
        case 0x08: buttons = OGC_BTN_DPAD_UP | OGC_BTN_DPAD_LEFT; return true;
        default: return false;
    }
}

const InterfaceInfo* match(const DeviceInfo& dev) {
    if (!dev.bluetooth || dev.vid != kGbrosVendorId || dev.pid != kGbrosBluetoothProductId ||
        dev.numInterfaces == 0 || dev.interfaces[0].inEndpoint == 0 ||
        dev.interfaces[0].inMaxPacket != kInputReportSize)
        return nullptr;
    return &dev.interfaces[0];
}

bool parse(const uint8_t* data, int len, ogc_state& state) {
    if (!data || len <= 0)
        return false;

    if (data[0] == 0x02) {
        if (len != 2 || data[1] > 1)
            return false;
        if (data[1])
            state.buttons |= OGC_BTN_GUIDE;
        else
            state.buttons &= ~static_cast<uint32_t>(OGC_BTN_GUIDE);
        return true;
    }

    if (data[0] != kInputReportId || len != kInputReportSize)
        return false;
    if ((data[10] & 0xFC) != 0 || (data[12] & 0xFC) != 0)
        return false;
    if ((data[13] & 0xF0) != 0)
        return false;

    uint32_t buttons = 0;
    if (!dpad(data[13], buttons))
        return false;
    buttons |= state.buttons & OGC_BTN_GUIDE;
    if (data[14] & 0x01) buttons |= OGC_BTN_A;
    if (data[14] & 0x02) buttons |= OGC_BTN_B;
    if (data[14] & 0x04) buttons |= OGC_BTN_X;
    if (data[14] & 0x08) buttons |= OGC_BTN_Y;
    if (data[14] & 0x40) buttons |= OGC_BTN_RB;
    if (data[14] & 0x80) buttons |= OGC_BTN_START;

    state.buttons = buttons;
    state.lt = trigger(data, 9, 10, 0x10);
    state.rt = trigger(data, 11, 12, 0x20);
    state.lx = axis(data[1], data[2], false);
    state.ly = axis(data[3], data[4], true);
    state.rx = axis(data[5], data[6], false);
    state.ry = axis(data[7], data[8], true);
    return true;
}

}  // namespace

const Driver kGbrosBluetoothDriver = {"gbros-bt-xinput", match, parse};

}  // namespace ogc
