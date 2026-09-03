#pragma once
#include <string>

// Compatibility wrapper for the process-wide persistent device identity.

namespace gasoline {
    std::string get_my_device_id();
}