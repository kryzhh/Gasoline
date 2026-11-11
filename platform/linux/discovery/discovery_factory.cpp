#include "discovery/discovery_interface.hpp"
#include "avahi_publisher.hpp"
#include "avahi_browser.hpp"

#include <memory>

namespace gasoline {

std::unique_ptr<IDiscoveryPublisher> create_platform_discovery_publisher() {
    return std::make_unique<AvahiPublisher>();
}

std::unique_ptr<IDiscoveryBrowser> create_platform_discovery_browser() {
    return std::make_unique<AvahiBrowser>();
}

} // namespace gasoline

