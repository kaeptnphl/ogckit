// OGCKit core: hotplug by rescanning, one reader thread per pad. USB access goes through ogc::Backend.
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <system_error>
#include <thread>

#include "internal.h"

namespace ogc {
namespace {

constexpr unsigned kScanIntervalUs = 1000000;
constexpr int kReadTimeoutMs = 250;
constexpr uint64_t kOpenRetryMs = 30000;

struct OpenFailure {
    bool used = false;
    uint16_t key = 0;
    uint64_t failedAtMs = 0;
};

OpenFailure g_openFailures[16];

const Driver* const kDrivers[] = {&kXInputDriver, &kGbrosBluetoothDriver,
                                  &kSwitchProBluetoothDriver, &kSwitchProUsbDriver};

struct Slot {
    bool used = false;
    bool alive = false;  // reader thread running
    bool stop = false;
    uint16_t key = 0;
    const Driver* driver = nullptr;
    InterfaceInfo itf = {};
    ogc_info info = {};
    ogc_state state = {};
    void* handle = nullptr;
    std::thread thread;
};

std::mutex g_mutex;
std::mutex g_lifecycleMutex;
Slot g_slots[OGC_MAX_PADS];
std::thread g_scanThread;
bool g_running = false;
bool g_inited = false;
void (*g_log)(const char*) = nullptr;

void resetSlot(Slot& s) {
    s.used = false;
    s.alive = false;
    s.stop = false;
    s.key = 0;
    s.driver = nullptr;
    s.itf = {};
    s.info = {};
    s.state = {};
    s.handle = nullptr;
    s.thread = std::thread();
}

int startThread(std::thread& thread, void* (*entry)(void*), void* arg) {
    try {
        thread = std::thread([entry, arg] { (void)entry(arg); });
        return 0;
    } catch (const std::system_error& e) {
        return e.code().value();
    }
}

void joinThread(std::thread& thread) {
    if (thread.joinable())
        thread.join();
}

}  // namespace

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void logf(const char* fmt, ...) {
    if (!g_log)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(buf);
}

namespace {

void* readerMain(void* arg) {
    Slot* s = static_cast<Slot*>(arg);
    uint8_t buf[512];
    const int len = s->itf.inMaxPacket > 0 && s->itf.inMaxPacket < sizeof(buf) ? s->itf.inMaxPacket
                                                                              : sizeof(buf);
    for (;;) {
        g_mutex.lock();
        const bool stop = s->stop;
        g_mutex.unlock();
        if (stop)
            break;
        const int got = kBackend.read(s->handle, s->itf.inEndpoint, buf, len, kReadTimeoutMs);
        if (got > 0) {
            g_mutex.lock();
            const bool parsed = s->driver->parse(buf, got, s->state);
            if (parsed)
                s->state.sequence++;
            g_mutex.unlock();
        } else if (got < 0) {
            logf("ogckit: %04X:%04X read error %d, dropping pad", s->info.vid, s->info.pid, got);
            break;
        }
    }
    kBackend.close(s->handle, s->itf.number);
    g_mutex.lock();
    s->alive = false;
    g_mutex.unlock();
    return nullptr;
}

bool deferOpen(uint16_t key, uint64_t now) {
    for (const OpenFailure& f : g_openFailures)
        if (f.used && f.key == key && now - f.failedAtMs < kOpenRetryMs)
            return true;
    return false;
}

void noteOpenFailure(uint16_t key, uint64_t now) {
    OpenFailure* oldest = &g_openFailures[0];
    for (OpenFailure& f : g_openFailures) {
        if (!f.used || f.key == key) {
            oldest = &f;
            break;
        }
        if (f.failedAtMs < oldest->failedAtMs)
            oldest = &f;
    }
    *oldest = {true, key, now};
}

void visit(void* dev, uint16_t key, const DeviceInfo& di, void*) {
    const Driver* driver = nullptr;
    const InterfaceInfo* itf = nullptr;
    for (const Driver* d : kDrivers) {
        if ((itf = d->match(di))) {
            driver = d;
            break;
        }
    }
    if (!driver)
        return;

    int free = -1;
    g_mutex.lock();
    if (!g_running) {
        g_mutex.unlock();
        return;
    }
    for (int i = 0; i < OGC_MAX_PADS; i++) {
        if (!g_slots[i].used) {
            free = free < 0 ? i : free;
        } else if (g_slots[i].key == key) {
            g_mutex.unlock();
            return;
        }
    }
    g_mutex.unlock();
    if (free < 0) {
        logf("ogckit: no free slot for %04X:%04X", di.vid, di.pid);
        return;
    }

    const uint64_t now = kBackend.monotonicMs();
    if (deferOpen(key, now))
        return;
    void* h = kBackend.open(dev, itf->number);
    if (!h) {
        logf("ogckit: open %04X:%04X failed; retry in 30s", di.vid, di.pid);
        noteOpenFailure(key, kBackend.monotonicMs());
        return;
    }
    for (OpenFailure& f : g_openFailures)
        if (f.used && f.key == key)
            f = OpenFailure();

    Slot& s = g_slots[free];
    g_mutex.lock();
    if (!g_running) {
        g_mutex.unlock();
        kBackend.close(h, itf->number);
        return;
    }
    resetSlot(s);
    s.used = true;
    s.alive = true;
    s.key = key;
    s.driver = driver;
    s.itf = *itf;
    s.handle = h;
    s.info.vid = di.vid;
    s.info.pid = di.pid;
    snprintf(s.info.driver, sizeof(s.info.driver), "%s", driver->name);
    const int threadError = startThread(s.thread, readerMain, &s);
    if (threadError != 0) {
        resetSlot(s);
        g_mutex.unlock();
        logf("ogckit: reader thread for %04X:%04X failed (%d)", di.vid, di.pid, threadError);
        kBackend.close(h, itf->number);
        return;
    }
    g_mutex.unlock();
    logf("ogckit: slot %d: %04X:%04X (%s)", free, di.vid, di.pid, driver->name);
}

// Frees slots whose reader thread has ended (pad unplugged or read error).
void reap() {
    for (int i = 0; i < OGC_MAX_PADS; i++) {
        g_mutex.lock();
        const bool dead = g_slots[i].used && !g_slots[i].alive;
        g_mutex.unlock();
        if (!dead)
            continue;
        joinThread(g_slots[i].thread);
        logf("ogckit: slot %d released", i);
        g_mutex.lock();
        resetSlot(g_slots[i]);
        g_mutex.unlock();
    }
}

void* scanMain(void*) {
    for (;;) {
        g_mutex.lock();
        const bool running = g_running;
        g_mutex.unlock();
        if (!running)
            break;
        reap();
        // A pad that vanished from the bus ends through its read error.
        kBackend.scan(visit, nullptr);
        kBackend.sleepUs(kScanIntervalUs);
    }
    return nullptr;
}

}  // namespace
}  // namespace ogc

extern "C" {

void ogc_set_log(void (*fn)(const char*)) {
    using namespace ogc;
    g_lifecycleMutex.lock();
    if (g_inited)
        logf("ogckit: set_log ignored while initialized");
    else
        g_log = fn;
    g_lifecycleMutex.unlock();
}

int ogc_init(void) {
    using namespace ogc;
    g_lifecycleMutex.lock();
    if (g_inited) {
        g_lifecycleMutex.unlock();
        return 0;
    }
    int r = kBackend.init(g_log);
    if (r < 0) {
        logf("ogckit: backend %s init failed (%d)", kBackend.name, r);
        g_lifecycleMutex.unlock();
        return r;
    }
    for (OpenFailure& f : g_openFailures)
        f = OpenFailure();
    g_mutex.lock();
    g_running = true;
    g_mutex.unlock();
    const int threadError = startThread(g_scanThread, scanMain, nullptr);
    if (threadError != 0) {
        logf("ogckit: scanner thread failed (%d)", threadError);
        g_mutex.lock();
        g_running = false;
        g_mutex.unlock();
        kBackend.exit();
        g_lifecycleMutex.unlock();
        return -1;
    }
    g_inited = true;
    g_lifecycleMutex.unlock();
    return 0;
}

void ogc_shutdown(void) {
    using namespace ogc;
    g_lifecycleMutex.lock();
    if (!g_inited) {
        g_lifecycleMutex.unlock();
        return;
    }
    g_mutex.lock();
    g_running = false;
    g_mutex.unlock();
    // Drain every scan/open before stopping readers: no slot can be reset or admitted afterward.
    joinThread(g_scanThread);
    g_mutex.lock();
    for (Slot& s : g_slots)
        s.stop = true;
    g_mutex.unlock();
    for (Slot& s : g_slots)
        if (s.used)
            joinThread(s.thread);
    g_mutex.lock();
    for (Slot& s : g_slots)
        resetSlot(s);
    g_mutex.unlock();
    kBackend.exit();
    g_inited = false;
    g_lifecycleMutex.unlock();
}

int ogc_get(int slot, ogc_info* info, ogc_state* state) {
    using namespace ogc;
    if (slot < 0 || slot >= OGC_MAX_PADS)
        return 0;
    g_mutex.lock();
    const Slot& s = g_slots[slot];
    const bool ok = g_running && s.used && s.alive;
    if (ok) {
        if (info)
            *info = s.info;
        if (state)
            *state = s.state;
    }
    g_mutex.unlock();
    return ok ? 1 : 0;
}

}  // extern "C"
