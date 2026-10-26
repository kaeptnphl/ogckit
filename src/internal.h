// Internal interfaces: a Driver understands one kind of pad, a Backend reaches USB.
#pragma once

#include <stdint.h>

#include "ogckit.h"

namespace ogc {

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

struct InterfaceInfo {
    uint8_t number;
    uint8_t cls, sub, proto;
    uint8_t inEndpoint;  // first interrupt-IN endpoint of alt setting 0, 0 if none
    uint16_t inMaxPacket;
};

enum Transport {
    kTransportUnknown = 0,
    kTransportUSB,
    kTransportBluetooth,
};

struct DeviceInfo {
    uint16_t vid, pid;
    uint8_t transport;     // Transport enum; USB interface ancestors imply kTransportUSB.
    uint8_t numInterfaces;
    InterfaceInfo interfaces[8];
};

struct Driver {
    const char* name;
    // Returns the interface to claim and read from, or nullptr if this driver does not fit.
    const InterfaceInfo* (*match)(const DeviceInfo& dev);
    // Applies one input report to state; false if the report is not an input report.
    bool (*parse)(const uint8_t* data, int len, ogc_state& state);
};

extern const Driver kXInputDriver;

struct Backend {
    const char* name;
    int (*init)(void (*log)(const char*));
    void (*exit)();
    // Calls visit for every attached device. `dev` is only valid during the call.
    void (*scan)(void (*visit)(void* dev, uint16_t key, const DeviceInfo& info, void* user), void* user);
    // Opens dev and claims the interface; returns a handle or nullptr.
    void* (*open)(void* dev, uint8_t interfaceNumber);
    // >0 bytes read, 0 timeout, <0 error (pad gone).
    int (*read)(void* handle, uint8_t endpoint, uint8_t* buf, int len, int timeoutMs);
    void (*close)(void* handle, uint8_t interfaceNumber);
    void (*sleepUs)(unsigned us);
    // Monotonic milliseconds, independent of wall-clock adjustments.
    uint64_t (*monotonicMs)();
};

extern const Backend kBackend;  // defined by exactly one backend_*.cpp per build

}  // namespace ogc
