#pragma once

#include "discovery_interface.hpp"

#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace gasoline {

class DiscoveryClient {
public:
    DiscoveryClient();
    explicit DiscoveryClient(std::unique_ptr<IDiscoveryBrowser> browser);
    ~DiscoveryClient();

    void start();
    void stop();

    void on_device_discovered(const DiscoveredDevice& device);

private:
    std::unique_ptr<IDiscoveryBrowser> browser_;
    std::mutex discovery_mutex_;
    std::set<std::string> pending_connections_;
};

} // namespace gasoline