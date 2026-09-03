#include <gasoline/discovery_service.hpp>
#include "discovery_interface.hpp"
#include <gasoline/config.hpp>
#include "utils/device_id.hpp"
#include "utils/logger.hpp"

#include <utility>

namespace gasoline {

DiscoveryService::DiscoveryService()
    : DiscoveryService(create_platform_discovery_publisher()) {}

DiscoveryService::DiscoveryService(std::unique_ptr<IDiscoveryPublisher> publisher)
    : publisher_(std::move(publisher)) {}

DiscoveryService::~DiscoveryService() {
    stop();
}

void DiscoveryService::start() {
    if (!publisher_) {
        log("DiscoveryService: No discovery publisher available");
        return;
    }

    const std::string my_id = get_my_device_id();
    log("DiscoveryService: Starting advertisement for device " + my_id + " on port " + std::to_string(SERVER_PORT));
    publisher_->start(my_id, SERVER_PORT);
}

void DiscoveryService::stop() {
    if (publisher_) {
        publisher_->stop();
    }
}

} // namespace gasoline

