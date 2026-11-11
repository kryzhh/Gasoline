#pragma once

#include "discovered_device.hpp"

#include <functional>
#include <memory>
#include <string>

namespace gasoline {

class IDiscoveryPublisher {
public:
    virtual ~IDiscoveryPublisher() = default;
    virtual bool start(const std::string& device_id, uint16_t port) = 0;
    virtual void stop() = 0;
};

class IDiscoveryBrowser {
public:
    using DiscoveryCallback = std::function<void(const DiscoveredDevice&)>;

    virtual ~IDiscoveryBrowser() = default;
    virtual bool start(DiscoveryCallback callback) = 0;
    virtual void stop() = 0;
};

std::unique_ptr<IDiscoveryPublisher> create_platform_discovery_publisher();
std::unique_ptr<IDiscoveryBrowser> create_platform_discovery_browser();

} // namespace gasoline

