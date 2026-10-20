// OGCKit core: hotplug by rescanning, one reader thread per pad. USB access goes through ogc::Backend.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

const Driver* const kDrivers[] = {&kXInputDriver};

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
    pthread_t thread = {};
};

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_lifecycleMutex = PTHREAD_MUTEX_INITIALIZER;
Slot g_slots[OGC_MAX_PADS];
pthread_t g_scanThread;
bool g_running = false;
bool g_inited = false;
void (*g_log)(const char*) = nullptr;

}  // namespace

__attribute__((format(printf, 1, 2))) void logf(const char* fmt, ...) {
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
    uint8_t buf[64];
    const int len = s->itf.inMaxPacket > 0 && s->itf.inMaxPacket < sizeof(buf) ? s->itf.inMaxPacket
                                                                              : sizeof(buf);
    for (;;) {
        pthread_mutex_lock(&g_mutex);
        const bool stop = s->stop;
        pthread_mutex_unlock(&g_mutex);
        if (stop)
            break;
        const int got = kBackend.read(s->handle, s->itf.inEndpoint, buf, len, kReadTimeoutMs);
        if (got > 0) {
            pthread_mutex_lock(&g_mutex);
            if (s->driver->parse(buf, got, s->state))
                s->state.sequence++;
            pthread_mutex_unlock(&g_mutex);
        } else if (got < 0) {
            logf("ogckit: %04X:%04X read error %d, dropping pad", s->info.vid, s->info.pid, got);
            break;
        }
    }
    kBackend.close(s->handle, s->itf.number);
    pthread_mutex_lock(&g_mutex);
    s->alive = false;
    pthread_mutex_unlock(&g_mutex);
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
    pthread_mutex_lock(&g_mutex);
    if (!g_running) {
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    for (int i = 0; i < OGC_MAX_PADS; i++) {
        if (!g_slots[i].used) {
            free = free < 0 ? i : free;
        } else if (g_slots[i].key == key) {
            pthread_mutex_unlock(&g_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&g_mutex);
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
    pthread_mutex_lock(&g_mutex);
    if (!g_running) {
        pthread_mutex_unlock(&g_mutex);
        kBackend.close(h, itf->number);
        return;
    }
    s = Slot();
    s.used = true;
    s.alive = true;
    s.key = key;
    s.driver = driver;
    s.itf = *itf;
    s.handle = h;
    s.info.vid = di.vid;
    s.info.pid = di.pid;
    snprintf(s.info.driver, sizeof(s.info.driver), "%s", driver->name);
    const int threadError = pthread_create(&s.thread, nullptr, readerMain, &s);
    if (threadError != 0) {
        s = Slot();
        pthread_mutex_unlock(&g_mutex);
        logf("ogckit: reader thread for %04X:%04X failed (%d)", di.vid, di.pid, threadError);
        kBackend.close(h, itf->number);
        return;
    }
    pthread_mutex_unlock(&g_mutex);
    logf("ogckit: slot %d: %04X:%04X (%s)", free, di.vid, di.pid, driver->name);
}

// Frees slots whose reader thread has ended (pad unplugged or read error).
void reap() {
    for (int i = 0; i < OGC_MAX_PADS; i++) {
        pthread_mutex_lock(&g_mutex);
        const bool dead = g_slots[i].used && !g_slots[i].alive;
        pthread_mutex_unlock(&g_mutex);
        if (!dead)
            continue;
        pthread_join(g_slots[i].thread, nullptr);
        logf("ogckit: slot %d released", i);
        pthread_mutex_lock(&g_mutex);
        g_slots[i] = Slot();
        pthread_mutex_unlock(&g_mutex);
    }
}

void* scanMain(void*) {
    for (;;) {
        pthread_mutex_lock(&g_mutex);
        const bool running = g_running;
        pthread_mutex_unlock(&g_mutex);
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
    pthread_mutex_lock(&g_lifecycleMutex);
    if (g_inited)
        logf("ogckit: set_log ignored while initialized");
    else
        g_log = fn;
    pthread_mutex_unlock(&g_lifecycleMutex);
}

int ogc_init(void) {
    using namespace ogc;
    pthread_mutex_lock(&g_lifecycleMutex);
    if (g_inited) {
        pthread_mutex_unlock(&g_lifecycleMutex);
        return 0;
    }
    int r = kBackend.init(g_log);
    if (r < 0) {
        logf("ogckit: backend %s init failed (%d)", kBackend.name, r);
        pthread_mutex_unlock(&g_lifecycleMutex);
        return r;
    }
    for (OpenFailure& f : g_openFailures)
        f = OpenFailure();
    pthread_mutex_lock(&g_mutex);
    g_running = true;
    pthread_mutex_unlock(&g_mutex);
    const int threadError = pthread_create(&g_scanThread, nullptr, scanMain, nullptr);
    if (threadError != 0) {
        logf("ogckit: scanner thread failed (%d)", threadError);
        pthread_mutex_lock(&g_mutex);
        g_running = false;
        pthread_mutex_unlock(&g_mutex);
        kBackend.exit();
        pthread_mutex_unlock(&g_lifecycleMutex);
        return -1;
    }
    g_inited = true;
    pthread_mutex_unlock(&g_lifecycleMutex);
    return 0;
}

void ogc_shutdown(void) {
    using namespace ogc;
    pthread_mutex_lock(&g_lifecycleMutex);
    if (!g_inited) {
        pthread_mutex_unlock(&g_lifecycleMutex);
        return;
    }
    pthread_mutex_lock(&g_mutex);
    g_running = false;
    pthread_mutex_unlock(&g_mutex);
    // Drain every scan/open before stopping readers: no slot can be reset or admitted afterward.
    pthread_join(g_scanThread, nullptr);
    pthread_mutex_lock(&g_mutex);
    for (Slot& s : g_slots)
        s.stop = true;
    pthread_mutex_unlock(&g_mutex);
    for (Slot& s : g_slots)
        if (s.used)
            pthread_join(s.thread, nullptr);
    pthread_mutex_lock(&g_mutex);
    for (Slot& s : g_slots)
        s = Slot();
    pthread_mutex_unlock(&g_mutex);
    kBackend.exit();
    g_inited = false;
    pthread_mutex_unlock(&g_lifecycleMutex);
}

int ogc_get(int slot, ogc_info* info, ogc_state* state) {
    using namespace ogc;
    if (slot < 0 || slot >= OGC_MAX_PADS)
        return 0;
    pthread_mutex_lock(&g_mutex);
    const Slot& s = g_slots[slot];
    const bool ok = g_running && s.used && s.alive;
    if (ok) {
        if (info)
            *info = s.info;
        if (state)
            *state = s.state;
    }
    pthread_mutex_unlock(&g_mutex);
    return ok ? 1 : 0;
}

}  // extern "C"
