// Tests based on captured GBros Bluetooth report payloads (no hardware needed).
#include <stdio.h>
#include <string.h>

#include "../src/internal.h"

int main() {
    using namespace ogc;
    int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

    CHECK(strlen(kGbrosBluetoothDriver.name) == 15);

    DeviceInfo device = {};
    device.vid = 0x045E;
    device.pid = 0x02E0;
    device.bluetooth = true;
    device.numInterfaces = 1;
    device.interfaces[0] = {0, 0, 0, 0, 0x81, 16};
    CHECK(kGbrosBluetoothDriver.match(device) == &device.interfaces[0]);
    device.bluetooth = false;
    CHECK(kGbrosBluetoothDriver.match(device) == nullptr);
    device.bluetooth = true;
    device.pid = 0x028E;
    CHECK(kGbrosBluetoothDriver.match(device) == nullptr);
    device.pid = 0x02E0;

    ogc_state state = {};
    const uint8_t neutral[16] = {
        0x01, 0x00, 0x80, 0x00, 0x80, 0xFF, 0x9B, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    CHECK(kGbrosBluetoothDriver.parse(neutral, sizeof(neutral), state));
    CHECK(state.buttons == 0 && state.lt == 0 && state.rt == 0);
    CHECK(state.lx == 0 && state.ly == 0 && state.rx == 7167 && state.ry == 0);

    uint8_t report[16];
    memcpy(report, neutral, sizeof(report));
    report[13] = 0x01;  // Captured d-pad up.
    report[14] = 0x0F;  // Captured A/B/X/Y bits.
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_DPAD_UP | OGC_BTN_A | OGC_BTN_B | OGC_BTN_X | OGC_BTN_Y));
    report[13] = 0;
    report[14] = 0x80;  // Captured Start bit.
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == OGC_BTN_START);

    report[13] = 0x03;
    report[14] = 0;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == OGC_BTN_DPAD_RIGHT);
    report[13] = 0x02;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_DPAD_UP | OGC_BTN_DPAD_RIGHT));
    report[13] = 0x04;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_DPAD_DOWN | OGC_BTN_DPAD_RIGHT));
    report[13] = 0x05;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == OGC_BTN_DPAD_DOWN);
    report[13] = 0x06;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_DPAD_DOWN | OGC_BTN_DPAD_LEFT));
    report[13] = 0x07;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == OGC_BTN_DPAD_LEFT);
    report[13] = 0x08;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_DPAD_UP | OGC_BTN_DPAD_LEFT));
    report[13] = 0;
    report[1] = 0xFF;
    report[2] = 0xFF;
    report[3] = 0;
    report[4] = 0;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.lx == 32767 && state.ly == 32767);
    report[1] = 0;
    report[2] = 0;
    report[3] = 0xFF;
    report[4] = 0xFF;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.lx == -32768 && state.ly == -32767);

    memcpy(report, neutral, sizeof(report));
    report[9] = 0x00;
    report[10] = 0x02;  // 512/1023, captured analog trigger range.
    report[11] = 0xFC;
    report[12] = 0x03;  // Captured maximum was 1020/1023.
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.lt == 128 && state.rt == 254);
    report[14] = 0x30;  // Captured independent L/R end-click bits.
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.lt == 255 && state.rt == 255);
    report[14] = 0x40;  // GBros Z maps to the existing right-shoulder semantic.
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == OGC_BTN_RB);
    report[14] = 0xC0;
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_RB | OGC_BTN_START));
    report[13] = 0x09;
    CHECK(!kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    report[13] = 0;

    const uint8_t homePress[2] = {0x02, 0x01};
    const uint8_t homeRelease[2] = {0x02, 0x00};
    CHECK(kGbrosBluetoothDriver.parse(homePress, sizeof(homePress), state));
    CHECK(state.buttons == (OGC_BTN_RB | OGC_BTN_START | OGC_BTN_GUIDE));
    memcpy(report, neutral, sizeof(report));
    CHECK(kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == OGC_BTN_GUIDE);
    CHECK(kGbrosBluetoothDriver.parse(homeRelease, sizeof(homeRelease), state));
    CHECK(state.buttons == 0);

    const ogc_state before = state;
    const uint8_t invalidHome[2] = {0x02, 0x02};
    CHECK(!kGbrosBluetoothDriver.parse(invalidHome, sizeof(invalidHome), state));
    const uint8_t battery[2] = {0x04, 0x7F};
    CHECK(!kGbrosBluetoothDriver.parse(battery, sizeof(battery), state));
    CHECK(!kGbrosBluetoothDriver.parse(neutral, sizeof(neutral) - 1, state));
    report[0] = 0x02;
    CHECK(!kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    report[0] = 0x01;
    report[10] = 0xFC;
    CHECK(!kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    report[10] = 0;
    report[13] = 0x10;
    CHECK(!kGbrosBluetoothDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == before.buttons && state.lt == before.lt && state.rt == before.rt);
    CHECK(state.lx == before.lx && state.ly == before.ly && state.rx == before.rx &&
          state.ry == before.ry);

    puts(fails ? "FAILED" : "ok");
    return fails != 0;
}
