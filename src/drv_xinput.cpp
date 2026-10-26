// XInput (Xbox 360 wired protocol): vendor class 0xFF, subclass 0x5D, protocol 0x01, interrupt IN
// endpoint with 20-byte reports. Layout as measured on an 8BitDo GBros. in XInput mode on PS4:
//   0: 0x00 (input report)  1: 0x14 (length)  2: d-pad, Start, Back, L3, R3  3: LB, RB, Guide, A B X Y
//   4: LT  5: RT  6..13: LX LY RX RY (int16 little endian, up positive)
//
// For Bluetooth HID devices, the report may be prefixed with a report ID byte. The parser accepts
// both formats: with or without the leading report ID.
#include "internal.h"

namespace ogc {

const InterfaceInfo* xinputMatch(const DeviceInfo& dev) {
    for (int i = 0; i < dev.numInterfaces && i < 8; i++) {
        const InterfaceInfo& itf = dev.interfaces[i];
        if (itf.cls == 0xFF && itf.sub == 0x5D && itf.proto == 0x01 && itf.inEndpoint)
            return &itf;
    }
    return nullptr;
}

static int16_t s16(const uint8_t* p) {
    return static_cast<int16_t>(p[0] | (p[1] << 8));
}

bool xinputParse(const uint8_t* d, int len, ogc_state& s) {
    // Handle report ID prefix if present (Bluetooth HID devices)
    // The report ID is prepended by the IOHID backend when reportID > 0
    const uint8_t* data = d;
    if (len > 14 && d[0] != 0x00) {
        // First byte is report ID, skip it
        data = d + 1;
        len--;
    }
    // Expect: data[0] == 0x00 (report type), data[1] == length (>= 14)
    if (len < 14 || data[0] != 0x00 || data[1] < 14 || data[1] > len)
        return false;
    struct Bit {
        int byte;
        uint8_t mask;
        uint32_t button;
    };
    static const Bit kBits[] = {
        {2, 0x01, OGC_BTN_DPAD_UP}, {2, 0x02, OGC_BTN_DPAD_DOWN},
        {2, 0x04, OGC_BTN_DPAD_LEFT}, {2, 0x08, OGC_BTN_DPAD_RIGHT},
        {2, 0x10, OGC_BTN_START}, {2, 0x20, OGC_BTN_BACK},
        {2, 0x40, OGC_BTN_L3}, {2, 0x80, OGC_BTN_R3},
        {3, 0x01, OGC_BTN_LB}, {3, 0x02, OGC_BTN_RB},
        {3, 0x04, OGC_BTN_GUIDE}, {3, 0x10, OGC_BTN_A},
        {3, 0x20, OGC_BTN_B}, {3, 0x40, OGC_BTN_X},
        {3, 0x80, OGC_BTN_Y},
    };
    uint32_t buttons = 0;
    for (const Bit& b : kBits)
        if (data[b.byte] & b.mask)
            buttons |= b.button;
    s.buttons = buttons;
    s.lt = data[4];
    s.rt = data[5];
    s.lx = s16(data + 6);
    s.ly = s16(data + 8);
    s.rx = s16(data + 10);
    s.ry = s16(data + 12);
    return true;
}

const Driver kXInputDriver = {"xinput", xinputMatch, xinputParse};

}  // namespace ogc
