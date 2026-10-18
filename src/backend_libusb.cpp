// libusb backend (Linux, macOS, Windows): for developing and testing without a console.
#include <libusb.h>
#include <unistd.h>

#include "internal.h"

namespace ogc {
namespace {

libusb_context* g_ctx = nullptr;

int init(void (*)(const char*)) {
    return libusb_init(&g_ctx);
}

void exit() {
    libusb_exit(g_ctx);
    g_ctx = nullptr;
}

bool describe(libusb_device* dev, DeviceInfo& out) {
    libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(dev, &dd) < 0)
        return false;
    out = {};
    out.vid = dd.idVendor;
    out.pid = dd.idProduct;
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) < 0 || !cfg)
        return false;
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
    libusb_free_config_descriptor(cfg);
    return true;
}

void scan(void (*visit)(void*, uint16_t, const DeviceInfo&, void*), void* user) {
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(g_ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        DeviceInfo di;
        if (!describe(list[i], di))
            continue;
        const uint16_t key = static_cast<uint16_t>((libusb_get_bus_number(list[i]) << 8) |
                                                   libusb_get_device_address(list[i]));
        visit(list[i], key, di, user);
    }
    if (list)
        libusb_free_device_list(list, 1);
}

void* open(void* dev, uint8_t interfaceNumber) {
    libusb_device_handle* h = nullptr;
    if (libusb_open(static_cast<libusb_device*>(dev), &h) < 0)
        return nullptr;
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, interfaceNumber) < 0) {
        libusb_close(h);
        return nullptr;
    }
    return h;
}

int read(void* handle, uint8_t ep, uint8_t* buf, int len, int timeoutMs) {
    int got = 0;
    const int r = libusb_interrupt_transfer(static_cast<libusb_device_handle*>(handle), ep, buf, len,
                                            &got, timeoutMs);
    if (r == 0)
        return got;
    return r == LIBUSB_ERROR_TIMEOUT ? 0 : r;
}

void close(void* handle, uint8_t interfaceNumber) {
    auto* h = static_cast<libusb_device_handle*>(handle);
    libusb_release_interface(h, interfaceNumber);
    libusb_close(h);
}

void sleepUs(unsigned us) {
    usleep(us);
}

}  // namespace

const Backend kBackend = {"libusb", ogc::init, ogc::exit, ogc::scan, ogc::open, ogc::read, ogc::close, ogc::sleepUs};

}  // namespace ogc
