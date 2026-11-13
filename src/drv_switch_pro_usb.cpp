// Switch-compatible 057E:2009 USB input reports.
#include "drv_switch_pro_common.h"

namespace ogc {
namespace {

const InterfaceInfo* matchUsb(const DeviceInfo& dev) {
    if (dev.bluetooth || dev.vid != kSwitchProVendorId || dev.pid != kSwitchProProductId)
        return nullptr;
    for (int i = 0; i < dev.numInterfaces && i < 8; ++i) {
        const InterfaceInfo& interface = dev.interfaces[i];
        if (interface.cls == kSwitchProHidInterfaceClass && interface.inEndpoint != 0 &&
            interface.inMaxPacket == kSwitchProObservedUsbInputReport)
            return &interface;
    }
    return nullptr;
}

bool parseUsb(const uint8_t* data, int len, ogc_state& state) {
    return data && len == kSwitchProUsbReportSize &&
           data[0] == kSwitchProFullInputReportId && parseFields(data, state);
}

}  // namespace

const Driver kSwitchProUsbDriver = {"switch-pro-usb", matchUsb, parseUsb};

}  // namespace ogc