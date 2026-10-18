// Parses recorded XInput reports (no hardware needed).
#include <stdio.h>
#include <string.h>

#include "../src/internal.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    using namespace ogc;
    ogc_state s;
    memset(&s, 0, sizeof(s));

    // A + Start + D-pad up, LT=255, left stick x = +32512, y = -32768.
    uint8_t r1[20] = {0x00, 0x14, 0x11, 0x10, 0xFF, 0x00, 0x00, 0x7F, 0x00, 0x80, 0, 0, 0, 0, 0};
    CHECK(kXInputDriver.parse(r1, sizeof(r1), s));
    CHECK(s.buttons == (OGC_BTN_A | OGC_BTN_START | OGC_BTN_DPAD_UP));
    CHECK(s.lt == 255 && s.rt == 0);
    CHECK(s.lx == 32512 && s.ly == -32768 && s.rx == 0 && s.ry == 0);

    // Y + RB + Guide + Back + R3.
    uint8_t r2[20] = {0x00, 0x14, 0xA0, 0x86, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kXInputDriver.parse(r2, sizeof(r2), s));
    CHECK(s.buttons == (OGC_BTN_Y | OGC_BTN_RB | OGC_BTN_GUIDE | OGC_BTN_BACK | OGC_BTN_R3));

    // Not input reports: wrong type, too short.
    uint8_t r3[20] = {0x01, 0x03, 0x0E};
    CHECK(!kXInputDriver.parse(r3, sizeof(r3), s));
    CHECK(!kXInputDriver.parse(r1, 8, s));

    // Interface matching.
    DeviceInfo di = {};
    di.numInterfaces = 2;
    di.interfaces[0] = {0, 0xFF, 0x5D, 0x01, 0x81, 32};
    di.interfaces[1] = {1, 0xFF, 0x5D, 0x03, 0x82, 32};
    CHECK(kXInputDriver.match(di) == &di.interfaces[0]);
    di.interfaces[0].proto = 0x02;
    CHECK(kXInputDriver.match(di) == nullptr);

    puts(fails ? "FAILED" : "ok");
    return fails != 0;
}
