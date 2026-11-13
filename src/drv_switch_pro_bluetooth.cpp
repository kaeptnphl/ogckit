// Switch-compatible 057E:2009 input reports observed over Bluetooth and USB.
#include "internal.h"

namespace ogc {
namespace {

constexpr uint16_t kVendorId = 0x057E;
constexpr uint16_t kProductId = 0x2009;
constexpr uint16_t kObservedMaxInputReport = 362;
constexpr uint16_t kObservedUsbInputReport = 64;
constexpr uint8_t kHidInterfaceClass = 0x03;
constexpr uint8_t kFullInputReportId = 0x30;
constexpr int kBluetoothReportSize = 49;
constexpr int kUsbReportSize = 64;

uint16_t stick12(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | ((data[1] & 0x0F) << 8));
}

uint16_t stick12High(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] >> 4) | (data[1] << 4));
}

int16_t normalizeStick(uint16_t raw) {
    int32_t value = (static_cast<int32_t>(raw) - 2048) * 16;
    if (value > 32767)
        value = 32767;
    if (value < -32768)
        value = -32768;
    return static_cast<int16_t>(value);
}

const InterfaceInfo* match(const DeviceInfo& dev) {
    if (!dev.bluetooth || dev.vid != kVendorId || dev.pid != kProductId ||
        dev.numInterfaces == 0 || dev.interfaces[0].inEndpoint == 0 ||
        dev.interfaces[0].inMaxPacket != kObservedMaxInputReport)
        return nullptr;
    return &dev.interfaces[0];
}

const InterfaceInfo* matchUsb(const DeviceInfo& dev) {
    if (dev.bluetooth || dev.vid != kVendorId || dev.pid != kProductId)
        return nullptr;
    for (int i = 0; i < dev.numInterfaces && i < 8; ++i) {
        const InterfaceInfo& interface = dev.interfaces[i];
        if (interface.cls == kHidInterfaceClass && interface.inEndpoint != 0 &&
            interface.inMaxPacket == kObservedUsbInputReport)
            return &interface;
    }
    return nullptr;
}

bool parseFields(const uint8_t* data, ogc_state& state) {
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

bool parseBluetooth(const uint8_t* data, int len, ogc_state& state) {
    return data && len == kBluetoothReportSize && data[0] == kFullInputReportId &&
           parseFields(data, state);
}

bool parseUsb(const uint8_t* data, int len, ogc_state& state) {
    return data && len == kUsbReportSize && data[0] == kFullInputReportId &&
           parseFields(data, state);
}

}  // namespace

const Driver kSwitchProBluetoothDriver = {"switch-pro-bt", match, parseBluetooth};
const Driver kSwitchProUsbDriver = {"switch-pro-usb", matchUsb, parseUsb};

}  // namespace ogc
