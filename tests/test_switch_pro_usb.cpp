// Parses captured Miadore Mode-A reports exposed as Switch Pro USB input.
#include <stdio.h>
#include <string.h>

#include "../src/internal.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void setLeftStick(uint8_t* r, uint16_t x, uint16_t y) {
    r[6] = static_cast<uint8_t>(x);
    r[7] = static_cast<uint8_t>(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    r[8] = static_cast<uint8_t>(y >> 4);
}

static void setRightStick(uint8_t* r, uint16_t x, uint16_t y) {
    r[9] = static_cast<uint8_t>(x);
    r[10] = static_cast<uint8_t>(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    r[11] = static_cast<uint8_t>(y >> 4);
}

static void idleReport(uint8_t* r) {
    memset(r, 0, 64);
    r[0] = 0x30;
    r[2] = 0x91;
    r[4] = 0x80;
    setLeftStick(r, 2048, 2048);
    setRightStick(r, 2048, 2048);
    r[12] = 0x0B;
}

int main() {
    using namespace ogc;
    ogc_state s = {};
    uint8_t report[64];
    idleReport(report);

    CHECK(kSwitchProUsbDriver.parse(report, sizeof(report), s));
    CHECK(s.buttons == 0 && s.lt == 0 && s.rt == 0);
    CHECK(s.lx == 0 && s.ly == 0 && s.rx == 0 && s.ry == 0);

    // Captured N64 controls: A/B, R/ZR, Start/Select/Home, Z/L, and the D-pad.
    report[3] = 0xCC;  // A, B, R, ZR
    report[4] = 0x93;  // Home, Start, Select, plus the constant baseline bit
    report[5] = 0xCF;  // Z, L, and all four D-pad directions
    CHECK(kSwitchProUsbDriver.parse(report, sizeof(report), s));
    CHECK(s.buttons == (OGC_BTN_A | OGC_BTN_B | OGC_BTN_RB | OGC_BTN_START |
                        OGC_BTN_BACK | OGC_BTN_GUIDE | OGC_BTN_LB | OGC_BTN_DPAD_UP |
                        OGC_BTN_DPAD_DOWN | OGC_BTN_DPAD_LEFT | OGC_BTN_DPAD_RIGHT));
    CHECK(s.lt == 255 && s.rt == 255);

    idleReport(report);
    report[3] = 0x80;  // ZR is the digital right-trigger state, not R.
    CHECK(kSwitchProUsbDriver.parse(report, sizeof(report), s));
    CHECK(s.buttons == 0 && s.lt == 0 && s.rt == 255);
    idleReport(report);
    report[5] = 0x80;  // Z is the digital left-trigger state.
    CHECK(kSwitchProUsbDriver.parse(report, sizeof(report), s));
    CHECK(s.buttons == 0 && s.lt == 255 && s.rt == 0);

    // Four C buttons are exposed as the second stick's cardinal directions.
    idleReport(report);
    setRightStick(report, 2048, 4080);
    CHECK(kSwitchProUsbDriver.parse(report, sizeof(report), s));
    CHECK(s.rx == 0 && s.ry == 32512);
    setRightStick(report, 4080, 2048);
    CHECK(kSwitchProUsbDriver.parse(report, sizeof(report), s));
    CHECK(s.rx == 32512 && s.ry == 0);
    setRightStick(report, 2048, 0);
    CHECK(kSwitchProUsbDriver.parse(report, sizeof(report), s));
    CHECK(s.rx == 0 && s.ry == -32768);
    setRightStick(report, 0, 2048);
    CHECK(kSwitchProUsbDriver.parse(report, sizeof(report), s));
    CHECK(s.rx == -32768 && s.ry == 0);

    // The main stick uses the first packed 12-bit X/Y pair, with up positive.
    idleReport(report);
    setLeftStick(report, 4080, 16);
    CHECK(kSwitchProUsbDriver.parse(report, sizeof(report), s));
    CHECK(s.lx == 32512 && s.ly == -32512);

    // Reject non-input, truncated, oversized, and null reports without changing state.
    const ogc_state before = s;
    report[0] = 0x21;
    CHECK(!kSwitchProUsbDriver.parse(report, sizeof(report), s));
    report[0] = 0x30;
    CHECK(!kSwitchProUsbDriver.parse(report, 63, s));
    CHECK(!kSwitchProUsbDriver.parse(report, 65, s));
    CHECK(!kSwitchProUsbDriver.parse(nullptr, sizeof(report), s));
    CHECK(s.buttons == before.buttons && s.lt == before.lt && s.rt == before.rt);
    CHECK(s.lx == before.lx && s.ly == before.ly && s.rx == before.rx && s.ry == before.ry);

    // Match only the observed USB interface and report size.
    DeviceInfo di = {};
    di.vid = 0x057E;
    di.pid = 0x2009;
    di.numInterfaces = 1;
    di.interfaces[0] = {0, 0x03, 0x00, 0x00, 0x81, 64};
    CHECK(kSwitchProUsbDriver.match(di) == &di.interfaces[0]);
    di.vid = 0x045E;
    CHECK(kSwitchProUsbDriver.match(di) == nullptr);
    di.vid = 0x057E;
    di.interfaces[0].inMaxPacket = 63;
    CHECK(kSwitchProUsbDriver.match(di) == nullptr);
    di.interfaces[0].inMaxPacket = 64;
    di.interfaces[0].cls = 0xFF;
    CHECK(kSwitchProUsbDriver.match(di) == nullptr);
    di.interfaces[0].cls = 0x03;
    di.interfaces[0].inEndpoint = 0;
    CHECK(kSwitchProUsbDriver.match(di) == nullptr);

    puts(fails ? "FAILED" : "ok");
    return fails != 0;
}
