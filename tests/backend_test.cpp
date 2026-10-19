// Fake USB backend: controls, snapshots and reader data share one mutex.
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include "test_backend.h"

namespace ogc {
namespace {

struct FakeDevice {
    uint16_t key = 0;
    DeviceInfo info = {};
    bool present = true;
    bool openFail = false;
    int readResult = 0;
    uint8_t readData[64] = {};
    int readLen = 0;
    int opens = 0, reads = 0, closes = 0;
};

std::mutex g_mutex;
std::condition_variable g_changed;
FakeDevice g_devices[16];
int g_numDevices = 0, g_initResult = 0, g_scans = 0;
int g_inits = 0, g_exits = 0, g_liveHandles = 0, g_readTimeout = 0;
bool g_active = false;
uint64_t g_nowMs = 0;
int g_blockOpen = -1;
bool g_openBlocked = false, g_blockScan = false, g_scanBlocked = false;

FakeDevice& device(int index) {
    assert(index >= 0 && index < g_numDevices);
    return g_devices[index];
}

template <typename Predicate>
bool wait(std::unique_lock<std::mutex>& lock, Predicate predicate) {
    return g_changed.wait_for(lock, std::chrono::seconds(2), predicate);
}

int test_init(void (*)(const char*)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_inits;
    g_active = g_initResult >= 0;
    return g_initResult;
}

void test_exit() {
    std::lock_guard<std::mutex> lock(g_mutex);
    assert(g_liveHandles == 0);
    ++g_exits;
    g_active = false;
}

void test_scan(void (*visit)(void*, uint16_t, const DeviceInfo&, void*), void* user) {
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_blockScan) {
        g_scanBlocked = true;
        g_changed.notify_all();
        g_changed.wait(lock, [] { return !g_blockScan; });
    }
    const int count = g_numDevices;
    for (int i = 0; i < count; ++i) {
        FakeDevice& d = g_devices[i];
        const bool present = d.present;
        const uint16_t key = d.key;
        const DeviceInfo info = d.info;
        lock.unlock();
        if (present)
            visit(&d, key, info, user);
        lock.lock();
    }
    ++g_scans;
    g_changed.notify_all();
}

void* test_open(void* dev, uint8_t) {
    std::unique_lock<std::mutex> lock(g_mutex);
    auto* d = static_cast<FakeDevice*>(dev);
    ++d->opens;
    if (g_blockOpen >= 0 && d == &g_devices[g_blockOpen]) {
        g_openBlocked = true;
        g_changed.notify_all();
        g_changed.wait(lock, [] { return g_blockOpen < 0; });
    }
    if (d->openFail || !d->present)
        return nullptr;
    ++g_liveHandles;
    return d;
}

int test_read(void* handle, uint8_t, uint8_t* buf, int len, int timeoutMs) {
    std::unique_lock<std::mutex> lock(g_mutex);
    auto* d = static_cast<FakeDevice*>(handle);
    ++d->reads;
    g_readTimeout = timeoutMs;
    g_changed.wait_for(lock, std::chrono::milliseconds(5),
                       [d] { return !d->present || d->readResult != 0; });
    if (!d->present)
        return -1;
    if (d->readResult <= 0)
        return d->readResult;
    const int n = d->readLen < len ? d->readLen : len;
    std::memcpy(buf, d->readData, n);
    d->readResult = 0;
    return n;
}

void test_close(void* handle, uint8_t) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++static_cast<FakeDevice*>(handle)->closes;
    --g_liveHandles;
    g_changed.notify_all();
}

void test_sleepUs(unsigned) {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_changed.wait_for(lock, std::chrono::milliseconds(1));
}

uint64_t test_monotonicMs() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_nowMs;
}

}  // namespace

const Backend kBackend = {"test", test_init, test_exit, test_scan, test_open,
                         test_read, test_close, test_sleepUs, test_monotonicMs};

namespace test_backend {

void reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    assert(!g_active && g_liveHandles == 0);
    for (FakeDevice& d : g_devices)
        d = FakeDevice();
    g_numDevices = g_initResult = g_scans = g_inits = g_exits = g_readTimeout = 0;
    g_nowMs = 0;
    g_blockOpen = -1;
    g_openBlocked = g_blockScan = g_scanBlocked = false;
}

void set_init_result(int code) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initResult = code;
}

int add_device(uint16_t key, uint16_t vid, uint16_t pid,
               const InterfaceInfo* interfaces, int numInterfaces) {
    std::lock_guard<std::mutex> lock(g_mutex);
    assert(g_numDevices < 16 && numInterfaces >= 0 && numInterfaces <= 8);
    const int index = g_numDevices++;
    FakeDevice& d = g_devices[index];
    d.key = key;
    d.info.vid = vid;
    d.info.pid = pid;
    d.info.numInterfaces = static_cast<uint8_t>(numInterfaces);
    for (int i = 0; i < numInterfaces; ++i)
        d.info.interfaces[i] = interfaces[i];
    return index;
}

void set_open_fail(int index, bool fail) {
    std::lock_guard<std::mutex> lock(g_mutex);
    device(index).openFail = fail;
}

void set_read_result(int index, int result, const uint8_t* data, int len) {
    std::lock_guard<std::mutex> lock(g_mutex);
    FakeDevice& d = device(index);
    assert(len >= 0 && len <= 64 && (result <= 0 || (data && len > 0)));
    d.readResult = result;
    d.readLen = len;
    if (result > 0)
        std::memcpy(d.readData, data, len);
    g_changed.notify_all();
}

void remove_device(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    device(index).present = false;
    g_changed.notify_all();
}

void reconnect_device(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    device(index).present = true;
    device(index).readResult = 0;
    g_changed.notify_all();
}

int scan_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_scans;
}

bool wait_for_scan(int count) {
    std::unique_lock<std::mutex> lock(g_mutex);
    return wait(lock, [count] { return g_scans >= count; });
}

int open_count(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return device(index).opens;
}

int read_count(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return device(index).reads;
}

int close_count(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return device(index).closes;
}

int read_timeout_ms() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_readTimeout;
}

int init_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_inits;
}

int exit_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_exits;
}

void advance_clock(uint64_t ms) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_nowMs += ms;
}

void block_open(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    device(index);
    g_blockOpen = index;
    g_openBlocked = false;
}

bool wait_for_open_blocked() {
    std::unique_lock<std::mutex> lock(g_mutex);
    return wait(lock, [] { return g_openBlocked; });
}

void release_open() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_blockOpen = -1;
    g_changed.notify_all();
}

void block_scan() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_blockScan = true;
    g_scanBlocked = false;
}

bool wait_for_scan_blocked() {
    std::unique_lock<std::mutex> lock(g_mutex);
    return wait(lock, [] { return g_scanBlocked; });
}

void release_scan() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_blockScan = false;
    g_changed.notify_all();
}

}  // namespace test_backend
}  // namespace ogc
