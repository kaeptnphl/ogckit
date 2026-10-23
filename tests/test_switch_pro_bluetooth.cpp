// Captured reports are represented by their first 13 captured bytes; the 49-byte
// arrays zero-pad the remaining captured-zero payload. Other modified reports are synthetic.
#include <stdio.h>
#include <string.h>

#include "../src/internal.h"

static bool parseCapturedPrefix(const uint8_t captured[13], ogc_state& state) {
    uint8_t report[49] = {};
    memcpy(report, captured, 13);
    return ogc::kSwitchProBluetoothDriver.parse(report, sizeof(report), state);
}

// Reduced USB fixtures retain the captured 13-byte header and zero-pad the unparsed tail.
static bool parseUsbCapturedPrefix(const uint8_t captured[13], ogc_state& state) {
    uint8_t report[64] = {};
    memcpy(report, captured, 13);
    return ogc::kSwitchProUsbDriver.parse(report, sizeof(report), state);
}

int main() {
    using namespace ogc;
    int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

    CHECK(strlen(kSwitchProBluetoothDriver.name) <= 15);

    DeviceInfo device = {};
    device.vid = 0x057E;
    device.pid = 0x2009;
    device.bluetooth = true;
    device.numInterfaces = 1;
    device.interfaces[0] = {0, 0, 0, 0, 0x81, 362};
    CHECK(kSwitchProBluetoothDriver.match(device) == &device.interfaces[0]);
    device.bluetooth = false;
    CHECK(kSwitchProBluetoothDriver.match(device) == nullptr);
    device.bluetooth = true;
    device.pid = 0x2006;
    CHECK(kSwitchProBluetoothDriver.match(device) == nullptr);
    device.pid = 0x2009;
    device.interfaces[0].inMaxPacket = 49;
    CHECK(kSwitchProBluetoothDriver.match(device) == nullptr);
    device.interfaces[0].inMaxPacket = 362;

    CHECK(kSwitchProUsbDriver.name != nullptr);
    device.bluetooth = false;
    device.interfaces[0] = {0, 0x03, 0, 0, 0x81, 64};
    CHECK(kSwitchProUsbDriver.match(device) == &device.interfaces[0]);
    device.interfaces[0].cls = 0;
    CHECK(kSwitchProUsbDriver.match(device) == nullptr);
    device.interfaces[0] = {0, 0x03, 0, 0, 0x81, 49};
    CHECK(kSwitchProUsbDriver.match(device) == nullptr);
    device.interfaces[0] = {0, 0x03, 0, 0, 0x81, 64};
    device.bluetooth = true;
    CHECK(kSwitchProUsbDriver.match(device) == nullptr);
    device.bluetooth = false;
    const InterfaceInfo unrelatedThenUsb[2] = {
        {0, 0xFF, 0, 0, 0x82, 32},
        {1, 0x03, 0, 0, 0x81, 64}
    };
    device.numInterfaces = 2;
    device.interfaces[0] = unrelatedThenUsb[0];
    device.interfaces[1] = unrelatedThenUsb[1];
    CHECK(kSwitchProUsbDriver.match(device) == &device.interfaces[1]);
    device.interfaces[1].inEndpoint = 0;
    CHECK(kSwitchProUsbDriver.match(device) == nullptr);
    device.interfaces[1] = unrelatedThenUsb[1];

    ogc_state state = {};
    const uint8_t capturedNeutral[49] = {
        0x30, 0xC2, 0x40, 0x00, 0x00, 0x00, 0xF0, 0x07, 0x7F, 0x70,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedNeutral, sizeof(capturedNeutral), state));
    CHECK(state.buttons == 0 && state.lt == 0 && state.rt == 0);
    CHECK(state.lx == -256 && state.ly == -256);
    CHECK(state.rx == 5888 && state.ry == -256);

    // Complete captured USB report 4432, reconstructed with its zero runs intact.
    uint8_t capturedUsbA[64] = {};
    const uint8_t capturedUsbAHeader[13] = {
        0x30, 0xEA, 0x70, 0x08, 0x00, 0x00, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B
    };
    memcpy(capturedUsbA, capturedUsbAHeader, sizeof(capturedUsbAHeader));
    capturedUsbA[51] = 0x78;
    capturedUsbA[52] = 0x29;
    capturedUsbA[54] = 0x20;
    capturedUsbA[55] = 0x68;
    capturedUsbA[58] = 0x20;
    capturedUsbA[59] = 0x01;
    capturedUsbA[63] = 0x7D;
    CHECK(kSwitchProUsbDriver.parse(capturedUsbA, sizeof(capturedUsbA), state));
    CHECK(state.buttons == OGC_BTN_A);
    CHECK(state.lx == -256 && state.ly == 0);
    CHECK(state.rx == -256 && state.ry == 0);
    // Complete captured USB report 1950: L shoulder, with the captured tail retained.
    const uint8_t capturedUsbL[64] = {
        0x30, 0xEA, 0x70, 0x00, 0x00, 0x40, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x78, 0x29, 0, 0x20, 0x68, 0, 0, 0x20,
        0x01, 0, 0, 0, 0x7D
    };
    CHECK(kSwitchProUsbDriver.parse(capturedUsbL, sizeof(capturedUsbL), state));
    CHECK(state.buttons == OGC_BTN_LB && state.lt == 0 && state.rt == 0);
    // Complete captured USB report 2662: R shoulder, with the same captured tail.
    const uint8_t capturedUsbR[64] = {
        0x30, 0x7A, 0x70, 0x40, 0x00, 0x00, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x78, 0x29, 0, 0x20, 0x68, 0, 0, 0x20,
        0x01, 0, 0, 0, 0x7D
    };
    CHECK(kSwitchProUsbDriver.parse(capturedUsbR, sizeof(capturedUsbR), state));
    CHECK(state.buttons == OGC_BTN_RB && state.lt == 0 && state.rt == 0);
    const uint8_t usbZL[13] = {
        0x30, 0x2C, 0x70, 0x00, 0x00, 0x80, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B
    };
    CHECK(parseUsbCapturedPrefix(usbZL, state) && state.lt == 255 && state.rt == 0);
    const uint8_t usbZR[13] = {
        0x30, 0x80, 0x70, 0x80, 0x00, 0x00, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B
    };
    CHECK(parseUsbCapturedPrefix(usbZR, state) && state.lt == 0 && state.rt == 255);
    const uint8_t usbDpadMasks[4][13] = {
        {0x30, 0xEE, 0x70, 0x00, 0x00, 0x02, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B},
        {0x30, 0x38, 0x70, 0x00, 0x00, 0x04, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B},
        {0x30, 0xB2, 0x70, 0x00, 0x00, 0x01, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B},
        {0x30, 0x02, 0x70, 0x00, 0x00, 0x08, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B}
    };
    const uint32_t usbDpadStandardButtons[4] = {
        OGC_BTN_DPAD_UP, OGC_BTN_DPAD_RIGHT, OGC_BTN_DPAD_DOWN, OGC_BTN_DPAD_LEFT
    };
    for (int i = 0; i < 4; ++i)
        CHECK(parseUsbCapturedPrefix(usbDpadMasks[i], state) &&
              state.buttons == usbDpadStandardButtons[i]);
    const uint8_t usbStickSample[13] = {
        0x30, 0xF4, 0x70, 0x00, 0x00, 0x00, 0xF0, 0x07, 0xFF, 0xF0, 0x07, 0x80, 0x0B
    };
    CHECK(parseUsbCapturedPrefix(usbStickSample, state));
    CHECK(state.lx == -256 && state.ly == 32512);
    CHECK(state.rx == -256 && state.ry == 0);
    const ogc_state usbBeforeInvalid = state;
    CHECK(!kSwitchProUsbDriver.parse(capturedUsbA, 63, state));
    capturedUsbA[0] = 0x31;
    CHECK(!kSwitchProUsbDriver.parse(capturedUsbA, sizeof(capturedUsbA), state));
    CHECK(state.buttons == usbBeforeInvalid.buttons && state.lt == usbBeforeInvalid.lt &&
          state.rt == usbBeforeInvalid.rt && state.lx == usbBeforeInvalid.lx &&
          state.ly == usbBeforeInvalid.ly && state.rx == usbBeforeInvalid.rx &&
          state.ry == usbBeforeInvalid.ry);

    // These fixtures retain the first 13 bytes of captured 49-byte reports;
    // their captured trailing 36 zero bytes are restored by parseCapturedPrefix.
    const uint8_t rvlA[13] = {0x30, 0x78, 0x40, 0x08, 0x00, 0x00, 0xF0, 0x07,
                              0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlA, state) && state.buttons == OGC_BTN_A);
    const uint8_t rvlB[13] = {0x30, 0xE6, 0x40, 0x04, 0x00, 0x00, 0xF0, 0x07,
                              0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlB, state) && state.buttons == OGC_BTN_B);
    const uint8_t rvlX[13] = {0x30, 0x5A, 0x40, 0x02, 0x00, 0x00, 0xF0, 0x07,
                              0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlX, state) && state.buttons == OGC_BTN_X);
    const uint8_t rvlY[13] = {0x30, 0xD2, 0x40, 0x01, 0x00, 0x00, 0xF0, 0x07,
                              0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlY, state) && state.buttons == OGC_BTN_Y);
    const uint8_t rvlMinus[13] = {0x30, 0xA0, 0x40, 0x00, 0x01, 0x00, 0xF0, 0x07,
                                  0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlMinus, state) && state.buttons == OGC_BTN_BACK);
    const uint8_t rvlPlus[13] = {0x30, 0x3C, 0x40, 0x00, 0x02, 0x00, 0xF0, 0x07,
                                 0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlPlus, state) && state.buttons == OGC_BTN_START);
    const uint8_t rvlHome[13] = {0x30, 0xE4, 0x40, 0x00, 0x10, 0x00, 0xF0, 0x07,
                                 0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlHome, state) && state.buttons == OGC_BTN_GUIDE);
    const uint8_t rvlZL[13] = {0x30, 0x8E, 0x40, 0x00, 0x00, 0x80, 0xF0, 0x07,
                               0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlZL, state) && state.lt == 255 && state.rt == 0);
    const uint8_t rvlZR[13] = {0x30, 0xF0, 0x40, 0x80, 0x00, 0x00, 0xF0, 0x07,
                               0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlZR, state) && state.lt == 0 && state.rt == 255);
    const uint8_t rvlDpadMasks[4][13] = {
        {0x30, 0x50, 0x40, 0x00, 0x00, 0x02, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B},
        {0x30, 0xD6, 0x40, 0x00, 0x00, 0x04, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B},
        {0x30, 0x42, 0x40, 0x00, 0x00, 0x01, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B},
        {0x30, 0xA2, 0x40, 0x00, 0x00, 0x08, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B}
    };
    const uint32_t rvlDpadStandardButtons[4] = {
        OGC_BTN_DPAD_UP, OGC_BTN_DPAD_RIGHT, OGC_BTN_DPAD_DOWN, OGC_BTN_DPAD_LEFT
    };
    for (int i = 0; i < 4; ++i)
        CHECK(parseCapturedPrefix(rvlDpadMasks[i], state) &&
              state.buttons == rvlDpadStandardButtons[i]);
    const uint8_t rvlStickExcursion[13] = {0x30, 0x4E, 0x40, 0x00, 0x00, 0x00, 0xF0,
                                           0x07, 0xFF, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlStickExcursion, state));
    CHECK(state.lx == -256 && state.ly == 32512);
    CHECK(state.rx == -256 && state.ry == 0);
    const uint8_t rvlL[13] = {0x30, 0x16, 0x40, 0x00, 0x00, 0x40, 0xF0, 0x07,
                              0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlL, state) && state.buttons == OGC_BTN_LB);
    const uint8_t rvlR[13] = {0x30, 0xFE, 0x40, 0x40, 0x00, 0x00, 0xF0, 0x07,
                              0x80, 0xF0, 0x07, 0x80, 0x0B};
    CHECK(parseCapturedPrefix(rvlR, state) && state.buttons == OGC_BTN_RB);

    const uint8_t capturedA[49] = {
        0x30, 0x08, 0x40, 0x08, 0x00, 0x00, 0xF0, 0x07, 0x7F, 0x70,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedA, sizeof(capturedA), state));
    CHECK(state.buttons == OGC_BTN_A);

    const uint8_t capturedPlus[49] = {
        0x30, 0x64, 0x40, 0x00, 0x02, 0x00, 0xF0, 0x07, 0x7F, 0x70,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedPlus, sizeof(capturedPlus), state));
    CHECK(state.buttons == OGC_BTN_START);
    const uint8_t capturedHome[49] = {
        0x30, 0x1E, 0x40, 0x00, 0x10, 0x00, 0xF0, 0x07, 0x7F, 0x90,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedHome, sizeof(capturedHome), state));
    CHECK(state.buttons == OGC_BTN_GUIDE);
    const uint8_t capturedCapture[49] = {
        0x30, 0x5E, 0x40, 0x00, 0x20, 0x00, 0xF0, 0x07, 0x7F, 0x70,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedCapture, sizeof(capturedCapture), state));
    CHECK(state.buttons == OGC_BTN_CAPTURE);
    const uint8_t capturedDpadUp[49] = {
        0x30, 0x36, 0x40, 0x00, 0x00, 0x02, 0xF0, 0x07, 0x7F, 0x70,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedDpadUp, sizeof(capturedDpadUp), state));
    CHECK(state.buttons == OGC_BTN_DPAD_UP);
    const uint8_t capturedLeftTrigger[49] = {
        0x30, 0xD0, 0x40, 0x00, 0x00, 0x80, 0xF0, 0x07, 0x7F, 0x30,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedLeftTrigger, sizeof(capturedLeftTrigger), state));
    CHECK(state.lt == 255 && state.rt == 0);
    const uint8_t capturedRightTrigger[49] = {
        0x30, 0x54, 0x40, 0x80, 0x00, 0x00, 0xF0, 0x07, 0x7F, 0x50,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedRightTrigger, sizeof(capturedRightTrigger), state));
    CHECK(state.lt == 0 && state.rt == 255);
    const uint8_t capturedSwitchProModeZ[49] = {
        0x30, 0x3C, 0x40, 0x80, 0x00, 0x00, 0xF0, 0x07, 0x7F, 0xB0,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedSwitchProModeZ, sizeof(capturedSwitchProModeZ), state));
    CHECK(state.buttons == 0 && state.lt == 0 && state.rt == 255);
    const uint8_t capturedSwitchProModeL[49] = {
        0x30, 0x3C, 0x40, 0x00, 0x00, 0x40, 0xF0, 0x07, 0x7F, 0xB0,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedSwitchProModeL, sizeof(capturedSwitchProModeL), state));
    CHECK(state.buttons == OGC_BTN_LB && state.lt == 0 && state.rt == 0);
    const uint8_t capturedSwitchProModeR[49] = {
        0x30, 0x3C, 0x40, 0x40, 0x00, 0x00, 0xF0, 0x07, 0x7F, 0xB0,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedSwitchProModeR, sizeof(capturedSwitchProModeR), state));
    CHECK(state.buttons == OGC_BTN_RB && state.lt == 0 && state.rt == 0);
    const uint8_t capturedSwitchProModeShare[49] = {
        0x30, 0x14, 0x40, 0x00, 0x20, 0x00, 0xF0, 0x07, 0x7F, 0xB0,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedSwitchProModeShare, sizeof(capturedSwitchProModeShare), state));
    CHECK(state.buttons == OGC_BTN_CAPTURE);
    CHECK((state.buttons & OGC_BTN_BACK) == 0);
    const uint8_t capturedGameCubeModeZ[49] = {
        0x30, 0x3C, 0x40, 0x40, 0x00, 0x00, 0xF0, 0x07, 0x7F, 0xB0,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedGameCubeModeZ, sizeof(capturedGameCubeModeZ), state));
    CHECK(state.buttons == OGC_BTN_RB && state.lt == 0 && state.rt == 0);
    const uint8_t capturedStickRight[49] = {
        0x30, 0x88, 0x40, 0x00, 0x00, 0x00, 0xF0, 0x0F, 0x7F, 0x70,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedStickRight, sizeof(capturedStickRight), state));
    CHECK(state.lx == 32512 && state.ly == -256);
    CHECK(state.rx == 5888 && state.ry == -256);
    const uint8_t capturedStickUp[49] = {
        0x30, 0x42, 0x40, 0x00, 0x00, 0x00, 0x10, 0x04, 0xFF, 0x70,
        0x09, 0x7F, 0x0B
    };
    CHECK(kSwitchProBluetoothDriver.parse(capturedStickUp, sizeof(capturedStickUp), state));
    CHECK(state.ly == 32512);

    uint8_t report[49];
    memcpy(report, capturedNeutral, sizeof(report));
    report[3] = 0x0F;  // Captured A/B/X/Y states, mapped per the standard report.
    CHECK(kSwitchProBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_A | OGC_BTN_B | OGC_BTN_X | OGC_BTN_Y));

    report[3] = 0x40;  // R maps to the existing right shoulder semantic.
    report[4] = 0x03;  // Minus/Plus map to Back/Start.
    report[5] = 0xC0;  // L bumper and digital ZL trigger.
    CHECK(kSwitchProBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_RB | OGC_BTN_BACK | OGC_BTN_START | OGC_BTN_LB));
    CHECK(state.lt == 255 && state.rt == 0);

    report[3] = 0x80;  // Digital ZR trigger.
    report[4] = 0x1C;  // Stick clicks and Home.
    report[5] = 0x00;
    CHECK(kSwitchProBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_L3 | OGC_BTN_R3 | OGC_BTN_GUIDE));
    CHECK(state.lt == 0 && state.rt == 255);

    report[3] = 0;
    report[4] = 0x20;  // Capture/Share is distinct from the Back button.
    report[5] = 0x0F;  // All D-pad directions can be held simultaneously.
    CHECK(kSwitchProBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_DPAD_UP | OGC_BTN_DPAD_DOWN |
                            OGC_BTN_DPAD_LEFT | OGC_BTN_DPAD_RIGHT | OGC_BTN_CAPTURE));

    report[4] = 0;
    report[5] = 0;
    report[6] = 0xFF;
    report[7] = 0x0F;
    report[8] = 0x00;  // Left stick X=4095, Y=0.
    report[9] = 0x00;
    report[10] = 0xF0;
    report[11] = 0xFF; // Right stick X=0, Y=4095.
    CHECK(kSwitchProBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.lx == 32752 && state.ly == -32768);
    CHECK(state.rx == -32768 && state.ry == 32752);

    const ogc_state before = state;
    report[0] = 0x31;
    CHECK(!kSwitchProBluetoothDriver.parse(report, sizeof(report), state));
    report[0] = 0x30;
    CHECK(!kSwitchProBluetoothDriver.parse(report, sizeof(report) - 1, state));
    CHECK(!kSwitchProBluetoothDriver.parse(nullptr, sizeof(report), state));
    CHECK(state.buttons == before.buttons && state.lt == before.lt &&
          state.rt == before.rt && state.lx == before.lx && state.ly == before.ly &&
          state.rx == before.rx && state.ry == before.ry);

    puts(fails ? "FAILED" : "ok");
    return fails != 0;
}
