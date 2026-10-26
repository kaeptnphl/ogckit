#include <cstdio>
#include <cstring>

#include "../src/internal.h"

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

void testXInputReport() {
    uint8_t report[20] = {
        0x00, 0x14, 0x11, 0x10, 0xFF, 0x00, 0x00, 0x7F, 0x00, 0x80,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    ogc_state state = {};
    CHECK(ogc::kXInputDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_A | OGC_BTN_START | OGC_BTN_DPAD_UP));
    CHECK(state.lt == 0xFF && state.rt == 0);
    CHECK(state.lx == 32512 && state.ly == -32768);
    CHECK(state.rx == 0 && state.ry == 0);
}

void testXInputReportWithReportId() {
    uint8_t report[21] = {
        0x01, 0x00, 0x14, 0x11, 0x10, 0xFF, 0x00, 0x00, 0x7F, 0x00, 0x80,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    ogc_state state = {};
    CHECK(ogc::kXInputDriver.parse(report, sizeof(report), state));
    CHECK(state.buttons == (OGC_BTN_A | OGC_BTN_START | OGC_BTN_DPAD_UP));
    CHECK(state.lt == 0xFF && state.rt == 0);
    CHECK(state.lx == 32512 && state.ly == -32768);
}

void testRejectsUnknownReport() {
    const uint8_t report[] = {0x00, 0x14, 0x00};
    ogc_state state = {};
    CHECK(!ogc::kXInputDriver.parse(report, sizeof(report), state));
}

}  // namespace

int main() {
    testXInputReport();
    testXInputReportWithReportId();
    testRejectsUnknownReport();
    if (failures == 0)
        std::puts("xinput tests passed");
    return failures != 0;
}