// PS4 backend (OpenOrbis / PacBrew toolchain): USB through sceUsbd, which has libusb's API.
#include <orbis/Sysmodule.h>
#include <orbis/Usbd.h>
#include <orbis/libkernel.h>
#include <chrono>

#include "internal.h"

// _types/libusb.h only forward-declares this; layout is libusb 1.0's.
struct libusb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    uint8_t bRefresh;
    uint8_t bSynchAddress;
    const unsigned char* extra;
    int32_t extra_length;
};

// Existing compatibility alias: the OpenOrbis header declares one argument.
// The actual sceUsbd ABI/ownership still needs confirmation before changing the unref policy.
extern "C" void ogc_free_device_list(libusb_device** list, int unref) __asm__("sceUsbdFreeDeviceList");

namespace ogc {
namespace {

// sceUsbd reports libusb error -N as 0x80240000 + N (timeout: 0x80240007).
bool isTimeout(int r) {
    return r == -7 || static_cast<uint32_t>(r) == 0x80240007u;
}

int init(void (*)(const char*)) {
    const int moduleResult = sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD);
    logf("ogckit: load USBD module = 0x%X", static_cast<unsigned>(moduleResult));
    // Preserve initialization when the module was already loaded.
    const int r = sceUsbdInit();
    if (r < 0)
        logf("ogckit: sceUsbdInit failed: 0x%X", static_cast<unsigned>(r));
    return r;
}

void exit() {
    const int r = sceUsbdExit();
    if (r < 0)
        logf("ogckit: sceUsbdExit failed: 0x%X", static_cast<unsigned>(r));
}

bool describe(libusb_device* dev, DeviceInfo& out) {
    libusb_device_descriptor dd = {};
    int r = sceUsbdGetDeviceDescriptor(dev, &dd);
    if (r < 0) {
        logf("ogckit: sceUsbdGetDeviceDescriptor failed: 0x%X", static_cast<unsigned>(r));
        return false;
    }
    out = {};
    out.vid = dd.idVendor;
    out.pid = dd.idProduct;
    libusb_config_descriptor* cfg = nullptr;
    r = sceUsbdGetActiveConfigDescriptor(dev, &cfg);
    if (r < 0 || !cfg) {
        logf("ogckit: sceUsbdGetActiveConfigDescriptor %04X:%04X failed: 0x%X, descriptor=%p",
             out.vid, out.pid, static_cast<unsigned>(r), static_cast<void*>(cfg));
        return false;
    }
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const libusb_interface& intf = cfg->interface[i];
        if (intf.num_altsetting < 1 || out.numInterfaces >= 8)
            continue;
        const libusb_interface_descriptor& d = intf.altsetting[0];
        InterfaceInfo& ii = out.interfaces[out.numInterfaces++];
        ii = {d.bInterfaceNumber, d.bInterfaceClass, d.bInterfaceSubClass, d.bInterfaceProtocol, 0, 0};
        for (int e = 0; e < d.bNumEndpoints; e++) {
            const libusb_endpoint_descriptor& ep = d.endpoint[e];
            if ((ep.bEndpointAddress & 0x80) && (ep.bmAttributes & 3) == 3) {
                ii.inEndpoint = ep.bEndpointAddress;
                ii.inMaxPacket = ep.wMaxPacketSize & 0x7FF;
                break;
            }
        }
    }
    r = sceUsbdFreeConfigDescriptor(cfg);
    if (r < 0)
        logf("ogckit: sceUsbdFreeConfigDescriptor failed: 0x%X", static_cast<unsigned>(r));
    return true;
}

void scan(void (*visit)(void*, uint16_t, const DeviceInfo&, void*), void* user) {
    libusb_device** list = nullptr;
    const int n = sceUsbdGetDeviceList(&list);
    if (n < 0)
        logf("ogckit: sceUsbdGetDeviceList failed: 0x%X", static_cast<unsigned>(n));
    for (int i = 0; i < n && list; i++) {
        DeviceInfo di;
        if (!describe(list[i], di))
            continue;
        const uint16_t key = static_cast<uint16_t>((sceUsbdGetBusNumber(list[i]) << 8) |
                                                   sceUsbdGetDeviceAddress(list[i]));
        visit(list[i], key, di, user);
    }
    // Preserve the existing ABI workaround; reference ownership is not verified on sceUsbd.
    if (list && n >= 0)
        ogc_free_device_list(list, 0);
}

void* open(void* dev, uint8_t interfaceNumber) {
    libusb_device_handle* h = nullptr;
    int r = sceUsbdOpen(static_cast<libusb_device*>(dev), &h);
    if (r < 0 || !h) {
        logf("ogckit: sceUsbdOpen failed: 0x%X, handle=%p",
             static_cast<unsigned>(r), static_cast<void*>(h));
        return nullptr;
    }
    r = sceUsbdClaimInterface(h, interfaceNumber);
    if (r < 0) {
        logf("ogckit: sceUsbdClaimInterface %u failed: 0x%X",
             interfaceNumber, static_cast<unsigned>(r));
        sceUsbdClose(h);
        return nullptr;
    }
    return h;
}

int read(void* handle, uint8_t ep, uint8_t* buf, int len, int timeoutMs) {
    int got = 0;
    const int r = sceUsbdInterruptTransfer(static_cast<libusb_device_handle*>(handle), ep, buf, len,
                                           &got, timeoutMs);
    if (r == 0)
        return got;
    if (!isTimeout(r))
        logf("ogckit: sceUsbdInterruptTransfer endpoint 0x%02X failed: 0x%X",
             ep, static_cast<unsigned>(r));
    return isTimeout(r) ? 0 : r;
}

void close(void* handle, uint8_t interfaceNumber) {
    auto* h = static_cast<libusb_device_handle*>(handle);
    const int r = sceUsbdReleaseInterface(h, interfaceNumber);
    if (r < 0)
        logf("ogckit: sceUsbdReleaseInterface %u failed: 0x%X",
             interfaceNumber, static_cast<unsigned>(r));
    sceUsbdClose(h);
}

void sleepUs(unsigned us) {
    sceKernelUsleep(us);
}

uint64_t monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

const Backend kBackend = {"ps4", ogc::init, ogc::exit, ogc::scan, ogc::open, ogc::read,
                         ogc::close, ogc::sleepUs, ogc::monotonicMs};

}  // namespace ogc
