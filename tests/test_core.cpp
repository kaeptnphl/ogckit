// Runtime regressions using the synchronized fake USB backend, without hardware.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test_backend.h"

namespace {
using namespace ogc::test_backend;
int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

template <typename Predicate>
bool waitUntil(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

int addPad(uint16_t key = 0x101) {
    const ogc::InterfaceInfo itf = {0, 0xFF, 0x5D, 0x01, 0x81, 32};
    return add_device(key, 0x045E, 0x028E, &itf, 1);
}

int addBluetoothPad(uint16_t key, uint16_t vid, uint16_t pid, uint16_t maxReport) {
    const ogc::InterfaceInfo itf = {0, 0, 0, 0, 0x81, maxReport};
    const int device = add_device(key, vid, pid, &itf, 1);
    set_bluetooth(device, true);
    return device;
}

std::mutex logMutex;
std::vector<std::string> logs;
void logger(const char* line) {
    // Snapshot access from the logger must not deadlock.
    ogc_get(0, nullptr, nullptr);
    std::lock_guard<std::mutex> lock(logMutex);
    logs.emplace_back(line);
}

bool logged(const char* text) {
    std::lock_guard<std::mutex> lock(logMutex);
    for (const auto& line : logs)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}

void lifecycle() {
    reset();
    ogc_shutdown();
    ogc_set_log(logger);
    set_init_result(-99);
    CHECK(ogc_init() == -99);
    CHECK(logged("init failed (-99)"));
    ogc_shutdown();
    CHECK(exit_count() == 0);
    set_init_result(0);
    for (int n = 0; n < 5; ++n) {
        CHECK(ogc_init() == 0);
        CHECK(ogc_init() == 0);
        ogc_set_log(nullptr);
        CHECK(logged("set_log ignored"));
        ogc_shutdown();
        ogc_shutdown();
    }
    CHECK(init_count() == 6);
    CHECK(exit_count() == 5);
    ogc_set_log(nullptr);

    reset();
    std::thread first([] { for (int i = 0; i < 10; ++i) { ogc_init(); ogc_shutdown(); } });
    std::thread second([] { for (int i = 0; i < 10; ++i) { ogc_init(); ogc_shutdown(); } });
    first.join();
    second.join();
    ogc_shutdown();
    CHECK(init_count() == exit_count());
}

void snapshotsAndSlots() {
    reset();
    for (int i = 0; i < OGC_MAX_PADS + 1; ++i)
        addPad(static_cast<uint16_t>(0x100 + i));
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    for (int i = 0; i < OGC_MAX_PADS; ++i) {
        ogc_info info = {};
        ogc_state state = {};
        CHECK(ogc_get(i, &info, &state) == 1);
        CHECK(info.vid == 0x045E && info.pid == 0x028E);
        CHECK(std::strcmp(info.driver, "xinput") == 0);
        CHECK(state.sequence == 0);
        CHECK(open_count(i) == 1);
    }
    CHECK(open_count(OGC_MAX_PADS) == 0);
    ogc_info info = {};
    info.vid = 123;
    ogc_state state = {};
    state.sequence = 456;
    CHECK(ogc_get(-1, &info, &state) == 0);
    CHECK(ogc_get(OGC_MAX_PADS, &info, &state) == 0);
    CHECK(info.vid == 123 && state.sequence == 456);

    std::atomic<bool> done{false};
    std::thread getter([&] {
        while (!done.load()) {
            ogc_info currentInfo = {};
            ogc_state currentState = {};
            ogc_get(0, &currentInfo, &currentState);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    ogc_shutdown();
    done.store(true);
    getter.join();
    CHECK(ogc_get(0, &info, &state) == 0);
    CHECK(info.vid == 123 && state.sequence == 456);
    for (int i = 0; i < OGC_MAX_PADS; ++i)
        CHECK(close_count(i) == 1);

    reset();
    const ogc::InterfaceInfo unsupported = {0, 0x03, 0, 0, 0x81, 32};
    const int d = add_device(0x101, 1, 2, &unsupported, 1);
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    CHECK(open_count(d) == 0);
    CHECK(ogc_get(0, nullptr, nullptr) == 0);
    ogc_shutdown();
}

void parserAndReconnect() {
    reset();
    const int d = addPad();
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    CHECK(waitUntil([&] { return read_count(d) > 0; }));
    CHECK(read_timeout_ms() == 250);
    uint8_t report[20] = {0x00, 0x14, 0x11, 0x10, 0xFF, 0x00, 0x00, 0x7F, 0x00, 0x80};
    set_read_result(d, 20, report, 20);
    ogc_state state = {};
    CHECK(waitUntil([&] { return ogc_get(0, nullptr, &state) && state.sequence == 1; }));
    CHECK(state.buttons == (OGC_BTN_A | OGC_BTN_START | OGC_BTN_DPAD_UP));
    CHECK(state.lt == 255 && state.rt == 0);
    CHECK(state.lx == 32512 && state.ly == -32768 && state.rx == 0 && state.ry == 0);
    const int before = read_count(d);
    report[0] = 1;
    set_read_result(d, 20, report, 20);
    CHECK(waitUntil([&] { return read_count(d) >= before + 2; }));
    CHECK(ogc_get(0, nullptr, &state) == 1);
    CHECK(state.sequence == 1 && state.lt == 255);

    remove_device(d);
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 0; }));
    CHECK(close_count(d) == 1);
    reconnect_device(d);
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 1; }));
    CHECK(ogc_get(0, nullptr, &state) == 1);
    CHECK(state.sequence == 0 && state.buttons == 0 && state.lt == 0);
    CHECK(open_count(d) == 2);
    ogc_shutdown();
    CHECK(close_count(d) == 2);
}

void switchProBluetoothCore() {
    reset();
    const int d = addBluetoothPad(0x201, 0x057E, 0x2009, 362);
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    CHECK(waitUntil([&] { return ogc_get(0, nullptr, nullptr) == 1; }));
    ogc_info info = {};
    ogc_state state = {};
    CHECK(ogc_get(0, &info, &state) == 1);
    CHECK(info.vid == 0x057E && info.pid == 0x2009);
    CHECK(std::strcmp(info.driver, "switch-pro-bt") == 0);
    CHECK(state.sequence == 0);

    // Reduced Bluetooth capture fixture: first 13 bytes from the report, zero-padded to 49.
    const uint8_t capturedA[49] = {
        0x30, 0x08, 0x40, 0x08, 0x00, 0x00, 0xF0, 0x07, 0x7F, 0x70,
        0x09, 0x7F, 0x0B
    };
    set_read_result(d, sizeof(capturedA), capturedA, sizeof(capturedA));
    CHECK(waitUntil([&] {
        return ogc_get(0, nullptr, &state) && state.sequence == 1;
    }));
    CHECK(state.buttons == OGC_BTN_A);
    CHECK(state.lx == -256 && state.ly == -256);
    CHECK(state.rx == 5888 && state.ry == -256);

    // Capture bit is a separate API button from Back, delivered from the captured report layout.
    const uint8_t capturedCapture[49] = {
        0x30, 0x5E, 0x40, 0x00, 0x20, 0x00, 0xF0, 0x07, 0x7F, 0x70,
        0x09, 0x7F, 0x0B
    };
    set_read_result(d, sizeof(capturedCapture), capturedCapture, sizeof(capturedCapture));
    CHECK(waitUntil([&] {
        return ogc_get(0, nullptr, &state) && state.sequence == 2;
    }));
    CHECK(state.buttons == OGC_BTN_CAPTURE);

    remove_device(d);
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 0; }));
    CHECK(waitUntil([&] { return close_count(d) == 1; }));
    reconnect_device(d);
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 1; }));
    CHECK(ogc_get(0, &info, &state) == 1);
    CHECK(info.vid == 0x057E && info.pid == 0x2009);
    CHECK(std::strcmp(info.driver, "switch-pro-bt") == 0);
    CHECK(state.sequence == 0 && state.buttons == 0 && state.lt == 0 && state.rt == 0);
    CHECK(open_count(d) == 2);
    ogc_shutdown();
    CHECK(close_count(d) == 2);
}

void switchProUsbCore() {
    reset();
    const ogc::InterfaceInfo interfaces[2] = {
        {0, 0xFF, 0, 0, 0x82, 32},
        {1, 0x03, 0, 0, 0x81, 64}
    };
    const int d = add_device(0x203, 0x057E, 0x2009, interfaces, 2);
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    CHECK(waitUntil([&] { return ogc_get(0, nullptr, nullptr) == 1; }));

    ogc_info info = {};
    ogc_state state = {};
    CHECK(ogc_get(0, &info, &state) == 1);
    CHECK(info.vid == 0x057E && info.pid == 0x2009);
    CHECK(std::strcmp(info.driver, "switch-pro-usb") == 0);

    // Full 64-byte USB report 4432 from the passive capture, with its zero runs retained.
    uint8_t capturedA[64] = {};
    const uint8_t header[13] = {
        0x30, 0xEA, 0x70, 0x08, 0x00, 0x00, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B
    };
    std::memcpy(capturedA, header, sizeof(header));
    capturedA[51] = 0x78;
    capturedA[52] = 0x29;
    capturedA[54] = 0x20;
    capturedA[55] = 0x68;
    capturedA[58] = 0x20;
    capturedA[59] = 0x01;
    capturedA[63] = 0x7D;
    set_read_result(d, sizeof(capturedA), capturedA, sizeof(capturedA));
    CHECK(waitUntil([&] {
        return ogc_get(0, nullptr, &state) && state.sequence == 1;
    }));
    CHECK(state.buttons == OGC_BTN_A);
    CHECK(state.lx == -256 && state.ly == 0);

    const uint8_t capturedDpadUp[64] = {
        0x30, 0xEE, 0x70, 0x00, 0x00, 0x02, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B
    };
    set_read_result(d, sizeof(capturedDpadUp), capturedDpadUp, sizeof(capturedDpadUp));
    CHECK(waitUntil([&] {
        return ogc_get(0, nullptr, &state) && state.sequence == 2;
    }));
    CHECK(state.buttons == OGC_BTN_DPAD_UP);

    const uint8_t capturedL[64] = {
        0x30, 0xEA, 0x70, 0x00, 0x00, 0x40, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x78, 0x29, 0, 0x20, 0x68, 0, 0, 0x20,
        0x01, 0, 0, 0, 0x7D
    };
    set_read_result(d, sizeof(capturedL), capturedL, sizeof(capturedL));
    CHECK(waitUntil([&] {
        return ogc_get(0, nullptr, &state) && state.sequence == 3;
    }));
    CHECK(state.buttons == OGC_BTN_LB && state.lt == 0 && state.rt == 0);

    const uint8_t capturedR[64] = {
        0x30, 0x7A, 0x70, 0x40, 0x00, 0x00, 0xF0, 0x07, 0x80, 0xF0, 0x07, 0x80, 0x0B,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x78, 0x29, 0, 0x20, 0x68, 0, 0, 0x20,
        0x01, 0, 0, 0, 0x7D
    };
    set_read_result(d, sizeof(capturedR), capturedR, sizeof(capturedR));
    CHECK(waitUntil([&] {
        return ogc_get(0, nullptr, &state) && state.sequence == 4;
    }));
    CHECK(state.buttons == OGC_BTN_RB && state.lt == 0 && state.rt == 0);

    remove_device(d);
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 0; }));
    CHECK(waitUntil([&] { return close_count(d) == 1; }));
    reconnect_device(d);
    CHECK(waitUntil([&] { return ogc_get(0, nullptr, nullptr) == 1; }));
    CHECK(ogc_get(0, &info, &state) == 1);
    CHECK(std::strcmp(info.driver, "switch-pro-usb") == 0);
    CHECK(state.sequence == 0 && state.buttons == 0 && state.lt == 0 && state.rt == 0);
    CHECK(open_count(d) == 2);
    ogc_shutdown();
    CHECK(close_count(d) == 2);
}

void gbrosGuideCoreMerge() {
    reset();
    const int d = addBluetoothPad(0x202, 0x045E, 0x02E0, 16);
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    CHECK(waitUntil([&] { return ogc_get(0, nullptr, nullptr) == 1; }));

    ogc_info info = {};
    ogc_state state = {};
    CHECK(ogc_get(0, &info, &state) == 1);
    CHECK(std::strcmp(info.driver, "gbros-bt-xinput") == 0);

    const uint8_t guidePress[2] = {0x02, 0x01};
    set_read_result(d, sizeof(guidePress), guidePress, sizeof(guidePress));
    CHECK(waitUntil([&] {
        return ogc_get(0, nullptr, &state) && state.sequence == 1;
    }));
    CHECK(state.buttons == OGC_BTN_GUIDE);

    const uint8_t gamepad[16] = {
        0x01, 0x00, 0x80, 0x00, 0x80, 0xFF, 0x9B, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00
    };
    set_read_result(d, sizeof(gamepad), gamepad, sizeof(gamepad));
    CHECK(waitUntil([&] {
        return ogc_get(0, nullptr, &state) && state.sequence == 2;
    }));
    CHECK(state.buttons == (OGC_BTN_GUIDE | OGC_BTN_A));

    const uint8_t guideRelease[2] = {0x02, 0x00};
    set_read_result(d, sizeof(guideRelease), guideRelease, sizeof(guideRelease));
    CHECK(waitUntil([&] {
        return ogc_get(0, nullptr, &state) && state.sequence == 3;
    }));
    CHECK(state.buttons == OGC_BTN_A);
    ogc_shutdown();
    CHECK(close_count(d) == 1);
}

void transportErrorRecovery() {
    reset();
    const int d = addPad();
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    block_scan();
    CHECK(wait_for_scan_blocked());
    set_read_result(d, -7);
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 0; }));
    CHECK(close_count(d) == 1);
    set_read_result(d, 0);
    release_scan();
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 1; }));
    CHECK(open_count(d) == 2);
    ogc_shutdown();
}

void retryAndRecovery() {
    reset();
    // Device key zero is valid; uptime exceeds the old unsigned-microsecond rollover.
    const int d = addPad(0);
    advance_clock(5000000);
    set_open_fail(d, true);
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    CHECK(open_count(d) == 1);
    set_open_fail(d, false);
    advance_clock(29999);
    CHECK(wait_for_scan(scan_count() + 2));
    CHECK(open_count(d) == 1);
    CHECK(ogc_get(0, nullptr, nullptr) == 0);
    advance_clock(1);
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 1; }));
    CHECK(open_count(d) == 2);
    ogc_shutdown();

    // An outstanding failure is forgotten by shutdown/reinit.
    reset();
    const int next = addPad();
    set_open_fail(next, true);
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    ogc_shutdown();
    set_open_fail(next, false);
    CHECK(ogc_init() == 0);
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 1; }));
    CHECK(open_count(next) == 2);
    ogc_shutdown();
}

void shutdownDuringOpen() {
    reset();
    const int first = addPad();
    const int second = addPad(0x102);
    block_open(second);
    CHECK(ogc_init() == 0);
    CHECK(wait_for_open_blocked());
    CHECK(ogc_get(0, nullptr, nullptr) == 1);
    std::thread shutdown([] { ogc_shutdown(); });
    // Shutdown has closed admission before the in-flight open returns.
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 0; }));
    release_open();
    shutdown.join();
    CHECK(read_count(second) == 0);
    CHECK(close_count(first) == 1 && close_count(second) == 1);
    CHECK(exit_count() == 1);
}

void shutdownDuringScan() {
    reset();
    const int d = addPad();
    CHECK(ogc_init() == 0);
    CHECK(wait_for_scan(1));
    block_scan();
    CHECK(wait_for_scan_blocked());
    const int next = addPad(0x102);
    std::thread shutdown([] { ogc_shutdown(); });
    CHECK(waitUntil([] { return ogc_get(0, nullptr, nullptr) == 0; }));
    release_scan();
    shutdown.join();
    CHECK(open_count(next) == 0);
    CHECK(close_count(d) == 1);
    CHECK(exit_count() == 1);
}
}  // namespace

int main(int argc, char** argv) {
    std::setbuf(stdout, nullptr);
    if (argc == 2 && std::strcmp(argv[1], "shutdown-open") == 0) {
        shutdownDuringOpen();
    } else {
        lifecycle();
        snapshotsAndSlots();
        parserAndReconnect();
        switchProBluetoothCore();
        switchProUsbCore();
        gbrosGuideCoreMerge();
        transportErrorRecovery();
        retryAndRecovery();
        shutdownDuringOpen();
        shutdownDuringScan();
    }
    std::printf("%s (%d failures)\n", fails ? "FAILED" : "ALL TESTS PASSED", fails);
    return fails != 0;
}
