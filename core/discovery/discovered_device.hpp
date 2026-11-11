#pragma once

#include <cstdint>
#include <string>

namespace gasoline {

struct DiscoveredDevice {
    std::string device_id;
    std::string device_name;
    std::string ip_address;
    uint16_t port{0};
};

} // namespace gasoline

