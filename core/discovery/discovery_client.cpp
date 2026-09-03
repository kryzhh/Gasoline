#include "discovery_client.hpp"

#include "device/device_registry.hpp"
#include "networking/connect.hpp"
#include "utils/device_id.hpp"
#include "utils/logger.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace gasoline {

namespace {

bool is_hex_digit(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool is_valid_uuid(const std::string& value) {
    if (value.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') {
                return false;
            }
        } else if (!is_hex_digit(value[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

DiscoveryClient::DiscoveryClient()
    : DiscoveryClient(create_platform_discovery_browser()) {}

DiscoveryClient::DiscoveryClient(std::unique_ptr<IDiscoveryBrowser> browser)
    : browser_(std::move(browser)) {}

DiscoveryClient::~DiscoveryClient() {
    stop();
}

void DiscoveryClient::start() {
    if (!browser_) {
        log("DiscoveryClient: No discovery browser available");
        return;
    }

    log("DiscoveryClient: Starting discovery browser...");
    browser_->start([this](const DiscoveredDevice& device) {
        on_device_discovered(device);
    });
}

void DiscoveryClient::stop() {
    if (browser_) {
        browser_->stop();
    }
}

void DiscoveryClient::on_device_discovered(const DiscoveredDevice& device) {
    // 1. Authoritative TXT device_id check: ignore if missing or invalid
    if (!is_valid_uuid(device.device_id)) {
        log("Discovered service '" + device.device_name + "' has invalid or missing device_id ('" +
            device.device_id + "'); ignoring");
        return;
    }

    // 2. Ignore our own service by comparing advertised UUID with local persistent UUID
    const std::string my_id = get_my_device_id();
    if (device.device_id == my_id) {
        log("DiscoveryClient: Discovered own service (" + my_id + "); ignoring");
        return;
    }

    // 3. Deduplicate by UUID: check if already connected or connection currently pending
    {
        std::lock_guard<std::mutex> lock(discovery_mutex_);
        if (device_registry.is_already_connected(device.device_id)) {
            log("Device " + device.device_id + " is already connected; skipping");
            return;
        }

        if (pending_connections_.count(device.device_id)) {
            log("Connection to device " + device.device_id + " is already pending; skipping");
            return;
        }

        pending_connections_.insert(device.device_id);
    }

    log("Discovered new device: " + device.device_id + " at " + device.ip_address + ":" + std::to_string(device.port));

    // 4. Connect asynchronously to avoid blocking the discovery callback
    std::thread([this, device_id = device.device_id, ip = device.ip_address, port = device.port]() {
        log("Attempting connection to: " + ip + ":" + std::to_string(port));
        connect_to_device(ip, port);

        // Allow time for handshake to complete or connect to fail
        std::this_thread::sleep_for(std::chrono::seconds(2));

        std::lock_guard<std::mutex> lock(discovery_mutex_);
        pending_connections_.erase(device_id);
    }).detach();
}

} // namespace gasoline