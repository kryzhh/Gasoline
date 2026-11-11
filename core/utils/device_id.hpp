#pragma once
#include <string>

// Compatibility wrapper for the process-wide persistent device identity.

namespace gasoline {
    class DeviceIdentity;
    const DeviceIdentity& get_my_device_identity();
    std::string get_my_device_id();
}
