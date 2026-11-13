// Switch-compatible 057E:2009 Bluetooth input reports.
#include "drv_switch_pro_common.h"

namespace ogc {
namespace {

const InterfaceInfo* match(const DeviceInfo& dev) {
    if (!dev.bluetooth || dev.vid != kSwitchProVendorId || dev.pid != kSwitchProProductId ||
        dev.numInterfaces == 0 || dev.interfaces[0].inEndpoint == 0 ||
        dev.interfaces[0].inMaxPacket != kSwitchProObservedMaxInputReport)
        return nullptr;
    return &dev.interfaces[0];
}

bool parseBluetooth(const uint8_t* data, int len, ogc_state& state) {
    return data && len == kSwitchProBluetoothReportSize &&
           data[0] == kSwitchProFullInputReportId && parseFields(data, state);
}

}  // namespace

const Driver kSwitchProBluetoothDriver = {"switch-pro-bt", match, parseBluetooth};

}  // namespace ogc
