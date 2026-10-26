// Switch Pro-style USB input exposed by the Miadore receiver in its Mode A setting.
#include "internal.h"

namespace ogc {
namespace {

constexpr uint16_t kNintendoVid = 0x057E;
constexpr uint16_t kSwitchProPid = 0x2009;
constexpr int kInputReportLength = 64;
constexpr uint16_t kAxisCenter = 2048;

uint16_t axisX(const uint8_t* d, int lowByte, int sharedByte) {
    return static_cast<uint16_t>(d[lowByte] | ((d[sharedByte] & 0x0F) << 8));
}

uint16_t axisY(const uint8_t* d, int sharedByte, int highByte) {
    return static_cast<uint16_t>((d[sharedByte] >> 4) | (d[highByte] << 4));
}

int16_t normalizeAxis(uint16_t value) {
    return static_cast<int16_t>((static_cast<int32_t>(value) - kAxisCenter) * 16);
}

}  // namespace

const InterfaceInfo* switchProUsbMatch(const DeviceInfo& dev) {
    if (dev.vid != kNintendoVid || dev.pid != kSwitchProPid)
        return nullptr;
    for (int i = 0; i < dev.numInterfaces && i < 8; ++i) {
        const InterfaceInfo& itf = dev.interfaces[i];
        if (itf.cls == 0x03 && itf.sub == 0x00 && itf.proto == 0x00 && itf.inEndpoint &&
            itf.inMaxPacket >= kInputReportLength)
            return &itf;
    }
    return nullptr;
}

bool switchProUsbParse(const uint8_t* d, int len, ogc_state& s) {
    if (!d || len != kInputReportLength || d[0] != 0x30)
        return false;

    const uint8_t buttons0 = d[3];
    const uint8_t buttons1 = d[4];
    const uint8_t buttons2 = d[5];
    uint32_t buttons = 0;

    if (buttons0 & 0x08)
        buttons |= OGC_BTN_A;
    if (buttons0 & 0x04)
        buttons |= OGC_BTN_B;
    if (buttons0 & 0x40)
        buttons |= OGC_BTN_RB;

    if (buttons1 & 0x02)
        buttons |= OGC_BTN_START;
    if (buttons1 & 0x01)
        buttons |= OGC_BTN_BACK;
    if (buttons1 & 0x10)
        buttons |= OGC_BTN_GUIDE;

    if (buttons2 & 0x40)
        buttons |= OGC_BTN_LB;
    if (buttons2 & 0x02)
        buttons |= OGC_BTN_DPAD_UP;
    if (buttons2 & 0x01)
        buttons |= OGC_BTN_DPAD_DOWN;
    if (buttons2 & 0x08)
        buttons |= OGC_BTN_DPAD_LEFT;
    if (buttons2 & 0x04)
        buttons |= OGC_BTN_DPAD_RIGHT;

    s.buttons = buttons;
    s.lt = (buttons2 & 0x80) ? 255 : 0;
    s.rt = (buttons0 & 0x80) ? 255 : 0;
    s.lx = normalizeAxis(axisX(d, 6, 7));
    s.ly = normalizeAxis(axisY(d, 7, 8));
    s.rx = normalizeAxis(axisX(d, 9, 10));
    s.ry = normalizeAxis(axisY(d, 10, 11));
    return true;
}

const Driver kSwitchProUsbDriver = {"switch-pro-usb", switchProUsbMatch, switchProUsbParse};

}  // namespace ogc
