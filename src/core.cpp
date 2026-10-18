// OGCKit core: hotplug by rescanning, one reader thread per pad. USB access goes through ogc::Backend.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

namespace ogc {
namespace {

constexpr unsigned kScanIntervalUs = 1000000;
constexpr int kReadTimeoutMs = 8;

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
Slot g_slots[OGC_MAX_PADS];
pthread_t g_scanThread;
bool g_running = false;
bool g_inited = false;
void (*g_log)(const char*) = nullptr;

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

    void* h = kBackend.open(dev, itf->number);
    if (!h) {
        logf("ogckit: open %04X:%04X failed", di.vid, di.pid);
        return;
    }

    Slot& s = g_slots[free];
    pthread_mutex_lock(&g_mutex);
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
    pthread_mutex_unlock(&g_mutex);
    if (pthread_create(&s.thread, nullptr, readerMain, &s) != 0) {
        logf("ogckit: reader thread for %04X:%04X failed", di.vid, di.pid);
        kBackend.close(h, itf->number);
        pthread_mutex_lock(&g_mutex);
        s = Slot();
        pthread_mutex_unlock(&g_mutex);
        return;
    }
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
    ogc::g_log = fn;
}

int ogc_init(void) {
    using namespace ogc;
    if (g_inited)
        return 0;
    int r = kBackend.init(g_log);
    if (r < 0) {
        logf("ogckit: backend %s init failed (%d)", kBackend.name, r);
        return r;
    }
    g_running = true;
    if (pthread_create(&g_scanThread, nullptr, scanMain, nullptr) != 0) {
        g_running = false;
        kBackend.exit();
        return -1;
    }
    g_inited = true;
    return 0;
}

void ogc_shutdown(void) {
    using namespace ogc;
    if (!g_inited)
        return;
    pthread_mutex_lock(&g_mutex);
    g_running = false;
    for (Slot& s : g_slots)
        s.stop = true;
    pthread_mutex_unlock(&g_mutex);
    pthread_join(g_scanThread, nullptr);
    for (Slot& s : g_slots)
        if (s.used)
            pthread_join(s.thread, nullptr);
    for (Slot& s : g_slots)
        s = Slot();
    kBackend.exit();
    g_inited = false;
}

int ogc_get(int slot, ogc_info* info, ogc_state* state) {
    using namespace ogc;
    if (slot < 0 || slot >= OGC_MAX_PADS)
        return 0;
    pthread_mutex_lock(&g_mutex);
    const Slot& s = g_slots[slot];
    const bool ok = s.used && s.alive;
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
