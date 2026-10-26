// macOS IOHID backend: reads pads that a macOS HID driver already owns, without root or detaching it.
//
// Observed on macOS with a GBros adapter (045E:028E): Apple's XboxGamepad driver publishes an
// IOHIDDevice directly under the FF/5D/01 USB interface. Its 20-byte input report has no report
// ID and keeps the raw XInput byte layout, so the unchanged XInput driver parses it. Other drivers
// or macOS versions may differ; reports the driver does not recognize are ignored, not guessed.
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <dispatch/dispatch.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "internal.h"
#include "report_queue.h"

namespace ogc {
namespace {

// IOHID has no endpoints; a non-zero IN endpoint lets drivers match. read() ignores it.
constexpr int kCancelWaitMs = 2000;

struct Handle {
    IOHIDDeviceRef device = nullptr;
    dispatch_queue_t queue = nullptr;
    ReportQueue reports;
    std::atomic<bool> loggedReportError{false};
    std::atomic<bool> loggedOversized{false};
    uint8_t buffer[ReportQueue::kMaxReport] = {};
};

bool intProperty(io_service_t service, CFStringRef key, bool searchParents, int& out) {
    CFTypeRef v = searchParents
        ? IORegistryEntrySearchCFProperty(service, kIOServicePlane, key, kCFAllocatorDefault,
                                          kIORegistryIterateRecursively | kIORegistryIterateParents)
        : IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    if (!v)
        return false;
    const bool ok = CFGetTypeID(v) == CFNumberGetTypeID() &&
                    CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &out);
    CFRelease(v);
    return ok;
}

void setInt(CFMutableDictionaryRef dict, CFStringRef key, int value) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    if (n) {
        CFDictionarySetValue(dict, key, n);
        CFRelease(n);
    }
}

// Gated by OGC_IOHID_DIAG=1, this logs the raw HID properties of every enumerated service so the
// Bluetooth discovery path can be verified without a parser or live report capture. It never
// changes which devices are described and never infers report formats.
static void logDiagnostic(io_service_t service) {
    int vid = 0, pid = 0, cls = 0, sub = 0, proto = 0, num = 0, maxIn = 0, maxOut = 0;
    CFTypeRef t = IORegistryEntryCreateCFProperty(service, CFSTR(kIOHIDTransportKey),
                                                  kCFAllocatorDefault, 0);
    std::string transport;
    if (t) {
        if (CFGetTypeID(t) == CFStringGetTypeID()) {
            CFStringRef s = static_cast<CFStringRef>(t);
            CFIndex n = CFStringGetLength(s) + 1;
            std::vector<char> buf(static_cast<size_t>(n));
            if (CFStringGetCString(s, buf.data(), n, kCFStringEncodingUTF8))
                transport = buf.data();
        }
        CFRelease(t);
    }
    intProperty(service, CFSTR(kIOHIDVendorIDKey), false, vid);
    intProperty(service, CFSTR(kIOHIDProductIDKey), false, pid);
    intProperty(service, CFSTR("bInterfaceClass"), true, cls);
    intProperty(service, CFSTR("bInterfaceSubClass"), true, sub);
    intProperty(service, CFSTR("bInterfaceProtocol"), true, proto);
    intProperty(service, CFSTR("bInterfaceNumber"), true, num);
    intProperty(service, CFSTR(kIOHIDMaxInputReportSizeKey), false, maxIn);
    intProperty(service, CFSTR(kIOHIDMaxOutputReportSizeKey), false, maxOut);
    logf("ogckit: iohid diag VID=0x%04X PID=0x%04X transport=%s bInterface=%d/%d/%d/%d "
         "maxIn=%d maxOut=%d",
         vid, pid, transport.empty() ? "(none)" : transport.c_str(), cls, sub, proto, num, maxIn,
         maxOut);
}

// Reads the transport family reported by the HID service itself. Returns false when the property
// is absent or not a string, so callers can fall back to USB-only behaviour.
bool transportString(io_service_t service, CFStringRef key, CFStringRef& out) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    if (!v)
        return false;
    const bool ok = CFGetTypeID(v) == CFStringGetTypeID();
    if (ok)
        out = static_cast<CFStringRef>(v);
    else
        CFRelease(v);
    return ok;
}

// HID devices may be backed by a USB interface or by Bluetooth. The interface triple is only
// meaningful when a USB interface ancestor exists; a Bluetooth HID gamepad has none, so the
// triple is left zeroed and the device is still described when its own HID properties prove it is
// a gamepad. The existing drivers decide whether to match on that basis.
bool describe(io_service_t service, DeviceInfo& out) {
    int vid = 0, pid = 0, num = 0, cls = 0, sub = 0, proto = 0, maxIn = 0;
    if (!intProperty(service, CFSTR(kIOHIDVendorIDKey), false, vid) ||
        !intProperty(service, CFSTR(kIOHIDProductIDKey), false, pid))
        return false;
    const bool hasUsbInterface =
        intProperty(service, CFSTR("bInterfaceClass"), true, cls) &&
        intProperty(service, CFSTR("bInterfaceSubClass"), true, sub) &&
        intProperty(service, CFSTR("bInterfaceProtocol"), true, proto) &&
        intProperty(service, CFSTR("bInterfaceNumber"), true, num);
    intProperty(service, CFSTR(kIOHIDMaxInputReportSizeKey), false, maxIn);
    out = {};
    out.vid = static_cast<uint16_t>(vid);
    out.pid = static_cast<uint16_t>(pid);
    out.numInterfaces = 1;
    if (hasUsbInterface) {
        // USB interface triple from the nearest IOUSBHostInterface ancestor.
        out.interfaces[0] = {static_cast<uint8_t>(num), static_cast<uint8_t>(cls),
                             static_cast<uint8_t>(sub), static_cast<uint8_t>(proto),
                             0x81, static_cast<uint16_t>(maxIn)};
    } else {
        // No USB interface: only describe the device when it is a Bluetooth HID gamepad, so the
        // existing drivers can still match it. The interface triple stays zeroed and the synthetic
        // IN endpoint is 0x81 so a driver that wants an endpoint can match.
        CFStringRef transport = nullptr;
        if (!transportString(service, CFSTR(kIOHIDTransportKey), transport))
            return false;
        const bool isBluetooth =
            CFStringCompare(transport, CFSTR("Bluetooth"), 0) == kCFCompareEqualTo;
        CFRelease(transport);
        if (!isBluetooth)
            return false;
        out.interfaces[0] = {0, 0, 0, 0, 0x81, static_cast<uint16_t>(maxIn)};
    }
    return true;
}

int init(void (*)(const char*)) {
    return 0;
}

void exit() {}

void scan(void (*visit)(void*, uint16_t, const DeviceInfo&, void*), void* user) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey);
    if (!match) {
        logf("ogckit: iohid matching dictionary allocation failed");
        return;
    }
    setInt(match, CFSTR(kIOHIDPrimaryUsagePageKey), kHIDPage_GenericDesktop);
    setInt(match, CFSTR(kIOHIDPrimaryUsageKey), kHIDUsage_GD_GamePad);
    io_iterator_t it = IO_OBJECT_NULL;
    const kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &it);  // consumes match
    if (kr != KERN_SUCCESS) {
        logf("ogckit: iohid service enumeration failed: 0x%08X", kr);
        return;
    }
    while (io_service_t service = IOIteratorNext(it)) {
        DeviceInfo di;
        uint64_t id = 0;
        if (getenv("OGC_IOHID_DIAG")) {
            logf("ogckit: iohid diag entry");
            logDiagnostic(service);
        }
        if (describe(service, di) && IORegistryEntryGetRegistryEntryID(service, &id) == KERN_SUCCESS) {
            // The registry entry ID is stable while the device stays attached and changes on replug.
            visit(reinterpret_cast<void*>(static_cast<uintptr_t>(service)),
                  static_cast<uint16_t>(id & 0xFFFF), di, user);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
}

void onReport(void* context, IOReturn result, void*, IOHIDReportType, uint32_t, uint8_t* report,
              CFIndex length) {
    auto* h = static_cast<Handle*>(context);
    if (result != kIOReturnSuccess) {
        if (!h->loggedReportError.exchange(true))
            logf("ogckit: iohid input report failed: 0x%08X", result);
        return;
    }
    if (length > ReportQueue::kMaxReport) {
        if (!h->loggedOversized.exchange(true))
            logf("ogckit: iohid dropped %ld-byte input report (limit %d)", static_cast<long>(length),
                 ReportQueue::kMaxReport);
        return;
    }
    h->reports.push(report, static_cast<int>(length));
}

void onRemoved(void* context, IOReturn, void*) {
    static_cast<Handle*>(context)->reports.markGone();
}

void* open(void* dev, uint8_t) {
    const auto service = static_cast<io_service_t>(reinterpret_cast<uintptr_t>(dev));
    int maxIn = 0;
    if (intProperty(service, CFSTR(kIOHIDMaxInputReportSizeKey), false, maxIn) &&
        maxIn > ReportQueue::kMaxReport) {
        logf("ogckit: iohid input report size %d exceeds limit %d", maxIn, ReportQueue::kMaxReport);
        return nullptr;
    }
    IOHIDDeviceRef device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
    if (!device) {
        logf("ogckit: iohid device create failed");
        return nullptr;
    }
    const IOReturn r = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        logf("ogckit: iohid open failed: 0x%08X%s", r,
             r == kIOReturnNotPermitted ? " (not permitted; check macOS privacy settings)" : "");
        CFRelease(device);
        return nullptr;
    }
    auto* h = new (std::nothrow) Handle;
    dispatch_queue_t queue = h ? dispatch_queue_create("ogckit.iohid", DISPATCH_QUEUE_SERIAL) : nullptr;
    if (!queue) {
        logf("ogckit: iohid handle allocation failed");
        delete h;
        IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
        CFRelease(device);
        return nullptr;
    }
    h->device = device;
    h->queue = queue;
    IOHIDDeviceRegisterInputReportCallback(device, h->buffer, sizeof(h->buffer), onReport, h);
    IOHIDDeviceRegisterRemovalCallback(device, onRemoved, h);
    IOHIDDeviceSetDispatchQueue(device, queue);
    IOHIDDeviceSetCancelHandler(device, ^{
        h->reports.markCancelled();
    });
    IOHIDDeviceActivate(device);
    return h;
}

int read(void* handle, uint8_t, uint8_t* buf, int len, int timeoutMs) {
    return static_cast<Handle*>(handle)->reports.pop(buf, len, timeoutMs);
}

void close(void* handle, uint8_t) {
    auto* h = static_cast<Handle*>(handle);
    // Callbacks reference h until the cancel handler has run on the device queue.
    IOHIDDeviceCancel(h->device);
    if (!h->reports.waitCancelled(kCancelWaitMs)) {
        logf("ogckit: iohid cancel did not complete within %d ms; leaking the device handle",
             kCancelWaitMs);
        return;
    }
    if (h->reports.dropped())
        logf("ogckit: iohid dropped %u stale input reports", h->reports.dropped());
    const IOReturn r = IOHIDDeviceClose(h->device, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess && r != kIOReturnNotAttached && r != kIOReturnNoDevice)
        logf("ogckit: iohid close failed: 0x%08X", r);
    CFRelease(h->device);
    dispatch_release(h->queue);
    delete h;
}

void sleepUs(unsigned us) {
    usleep(us);
}

uint64_t monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

const Backend kBackend = {"iohid", ogc::init, ogc::exit, ogc::scan, ogc::open, ogc::read,
                         ogc::close, ogc::sleepUs, ogc::monotonicMs};

}  // namespace ogc
