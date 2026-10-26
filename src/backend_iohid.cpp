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
#include <cstring>
#include <new>

#include "internal.h"
#include "report_queue.h"

namespace ogc {
namespace {

// IOHID has no endpoints; a non-zero IN endpoint lets drivers match. read() ignores it.
constexpr uint8_t kSyntheticInEndpoint = 0x81;
constexpr int kCancelWaitMs = 2000;

struct Handle {
    IOHIDDeviceRef device = nullptr;
    dispatch_queue_t queue = nullptr;
    ReportQueue reports;
    std::atomic<bool> loggedReportError{false};
    std::atomic<bool> loggedOversized{false};
    std::atomic<unsigned> reportCount{0};
    std::atomic<uint64_t> lastReportMs{0};
    std::atomic<bool> removalReported{false};
    uint64_t hidRegistryId = 0;
    uint64_t usbInterfaceRegistryId = 0;
    uint64_t usbDeviceRegistryId = 0;
    uint8_t buffer[ReportQueue::kMaxReport] = {};
};

struct UsbRegistryIds {
    uint64_t interfaceId = 0;
    uint64_t deviceId = 0;
};

uint64_t timestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

UsbRegistryIds usbRegistryIds(io_service_t service) {
    UsbRegistryIds ids;
    io_service_t parent = IO_OBJECT_NULL;
    if (IORegistryEntryGetParentEntry(service, kIOServicePlane, &parent) != KERN_SUCCESS)
        return ids;
    while (parent != IO_OBJECT_NULL) {
        char className[128] = {};
        uint64_t id = 0;
        if (IOObjectGetClass(parent, className) == KERN_SUCCESS) {
            const bool isUsbInterface =
                std::strcmp(className, "IOUSBHostInterface") == 0 ||
                std::strcmp(className, "IOUSBInterface") == 0;
            const bool isUsbDevice =
                std::strcmp(className, "IOUSBHostDevice") == 0 ||
                std::strcmp(className, "IOUSBDevice") == 0;
            if ((isUsbInterface && ids.interfaceId == 0) || (isUsbDevice && ids.deviceId == 0)) {
                if (IORegistryEntryGetRegistryEntryID(parent, &id) == KERN_SUCCESS) {
                    if (isUsbInterface)
                        ids.interfaceId = id;
                    else
                        ids.deviceId = id;
                }
            }
        }
        if (ids.interfaceId && ids.deviceId) {
            IOObjectRelease(parent);
            return ids;
        }
        io_service_t next = IO_OBJECT_NULL;
        const kern_return_t kr = IORegistryEntryGetParentEntry(parent, kIOServicePlane, &next);
        IOObjectRelease(parent);
        if (kr != KERN_SUCCESS)
            break;
        parent = next;
    }
    return ids;
}

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

// Only HID devices backed by a USB interface are described; the interface triple comes from the
// nearest IOUSBHostInterface ancestor so the existing drivers keep making the match decision.
bool describe(io_service_t service, DeviceInfo& out) {
    int vid = 0, pid = 0, num = 0, cls = 0, sub = 0, proto = 0, maxIn = 0;
    if (!intProperty(service, CFSTR(kIOHIDVendorIDKey), false, vid) ||
        !intProperty(service, CFSTR(kIOHIDProductIDKey), false, pid))
        return false;
    if (!intProperty(service, CFSTR("bInterfaceClass"), true, cls) ||
        !intProperty(service, CFSTR("bInterfaceSubClass"), true, sub) ||
        !intProperty(service, CFSTR("bInterfaceProtocol"), true, proto) ||
        !intProperty(service, CFSTR("bInterfaceNumber"), true, num))
        return false;
    intProperty(service, CFSTR(kIOHIDMaxInputReportSizeKey), false, maxIn);
    out = {};
    out.vid = static_cast<uint16_t>(vid);
    out.pid = static_cast<uint16_t>(pid);
    out.numInterfaces = 1;
    out.interfaces[0] = {static_cast<uint8_t>(num), static_cast<uint8_t>(cls),
                         static_cast<uint8_t>(sub), static_cast<uint8_t>(proto),
                         kSyntheticInEndpoint, static_cast<uint16_t>(maxIn)};
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
    io_iterator_t it = IO_OBJECT_NULL;
    const kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &it);  // consumes match
    if (kr != KERN_SUCCESS) {
        logf("ogckit: iohid service enumeration failed: 0x%08X", kr);
        return;
    }
    while (io_service_t service = IOIteratorNext(it)) {
        DeviceInfo di;
        int usagePage = 0, usage = 0;
        uint64_t id = 0;
        if (intProperty(service, CFSTR(kIOHIDPrimaryUsagePageKey), false, usagePage) &&
            intProperty(service, CFSTR(kIOHIDPrimaryUsageKey), false, usage) &&
            usagePage == kHIDPage_GenericDesktop &&
            (usage == kHIDUsage_GD_GamePad || usage == kHIDUsage_GD_Joystick) &&
            describe(service, di) && IORegistryEntryGetRegistryEntryID(service, &id) == KERN_SUCCESS) {
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
        logf(        "ogckit: iohid diag t=%llu report-error hid=%llu usb-interface=%llu usb-device=%llu result=0x%08X",
             static_cast<unsigned long long>(timestampMs()),
             static_cast<unsigned long long>(h->hidRegistryId),
        static_cast<unsigned long long>(h->usbInterfaceRegistryId),
        static_cast<unsigned long long>(h->usbDeviceRegistryId), result);
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
    const unsigned previousCount = h->reportCount.fetch_add(1);
    h->lastReportMs.store(timestampMs());
    if (previousCount == 0)
        logf("ogckit: iohid diag t=%llu first-report hid=%llu usb-interface=%llu usb-device=%llu len=%ld id=%u",
             static_cast<unsigned long long>(timestampMs()),
             static_cast<unsigned long long>(h->hidRegistryId),
             static_cast<unsigned long long>(h->usbInterfaceRegistryId),
             static_cast<unsigned long long>(h->usbDeviceRegistryId), static_cast<long>(length),
             length > 0 ? report[0] : 0);
    h->reports.push(report, static_cast<int>(length));
}

void onRemoved(void* context, IOReturn result, void*) {
    auto* h = static_cast<Handle*>(context);
    h->removalReported.store(true);
    const uint64_t lastReportMs = h->lastReportMs.load();
    logf("ogckit: iohid diag t=%llu removed hid=%llu usb-interface=%llu usb-device=%llu result=0x%08X reports=%u last-report-age-ms=%lld",
         static_cast<unsigned long long>(timestampMs()),
         static_cast<unsigned long long>(h->hidRegistryId),
         static_cast<unsigned long long>(h->usbInterfaceRegistryId),
         static_cast<unsigned long long>(h->usbDeviceRegistryId), result, h->reportCount.load(),
         static_cast<long long>(lastReportMs ? timestampMs() - lastReportMs : -1));
    h->reports.markGone();
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
    IORegistryEntryGetRegistryEntryID(service, &h->hidRegistryId);
    const UsbRegistryIds usbIds = usbRegistryIds(service);
    h->usbInterfaceRegistryId = usbIds.interfaceId;
    h->usbDeviceRegistryId = usbIds.deviceId;
    logf("ogckit: iohid diag t=%llu opened hid=%llu usb-interface=%llu usb-device=%llu",
         static_cast<unsigned long long>(timestampMs()),
         static_cast<unsigned long long>(h->hidRegistryId),
         static_cast<unsigned long long>(h->usbInterfaceRegistryId),
         static_cast<unsigned long long>(h->usbDeviceRegistryId));
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
    logf("ogckit: iohid diag t=%llu closing hid=%llu usb-interface=%llu usb-device=%llu removed=%d reports=%u",
         static_cast<unsigned long long>(timestampMs()),
         static_cast<unsigned long long>(h->hidRegistryId),
         static_cast<unsigned long long>(h->usbInterfaceRegistryId),
         static_cast<unsigned long long>(h->usbDeviceRegistryId), h->removalReported.load(),
         h->reportCount.load());
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
    logf("ogckit: iohid diag t=%llu close-result hid=%llu result=0x%08X",
         static_cast<unsigned long long>(timestampMs()),
         static_cast<unsigned long long>(h->hidRegistryId), r);
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
