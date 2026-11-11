#include "device_registry.hpp"
#include "../utils/logger.hpp"
#include "../networking/connection.hpp"
#include "../auth/session_authentication.hpp"

#include <algorithm>

namespace gasoline {

DeviceRegistry device_registry;

DeviceRegistry::RegistrationResult DeviceRegistry::publish_authenticated_device(
    const Device& device, const AuthorizedPeer& authorization) {
    // Release strong references after the registry lock: their destructors may
    // perform session cleanup, which also acquires this lock.
    auto candidate = device.connection.lock();
    std::shared_ptr<Connection> owner;
    std::lock_guard<std::mutex> lock(registry_mutex);
    if (!candidate || candidate->session_id() != device.session_id || candidate->is_stopping() ||
        device.device_id != authorization.device_id() ||
        device.public_key != authorization.public_key() ||
        device.trust_revision != authorization.revision() ||
        device.permissions != authorization.permissions() ||
        device.state != DeviceState::AUTHENTICATED) {
        return {};
    }
    for (auto& existing : devices) {
        if (existing.device_id != device.device_id) {
            continue;
        }
        owner = existing.connection.lock();
        // Keep the incumbent for same-direction duplicates. Only the preferred
        // UUID direction can replace a live nonpreferred owner.
        if (owner && !owner->is_stopping() &&
            (existing.preferred_connection || !device.preferred_connection)) {
            return {};
        }
        Device published = device;
        published.state = DeviceState::READY;
        RegistrationResult result{true, existing};
        existing = std::move(published);
        return result;
    }
    Device published = device;
    published.state = DeviceState::READY;
    devices.push_back(std::move(published));
    log("Device registered: " + device.device_name);
    return {true, std::nullopt};
}



void DeviceRegistry::remove_device(uint64_t session_id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    devices.erase(
        std::remove_if(devices.begin(), devices.end(), // Removes only if device actually exists
                       [session_id](const Device& d) {
                           return d.session_id == session_id;
                       }),
        devices.end()
    );
    log("Device removed from registry");
}

void DeviceRegistry::list_devices() { // Lists currently connected devices
    std::lock_guard<std::mutex> lock(registry_mutex);
    if (devices.empty()) {
        log("No connected devices");
        return;
    }
    log("Connected devices:");
    for (const auto& device : devices) {
        log(" - " + device.device_name +
            " (" + device.device_type + ")" +
            " [socket " + std::to_string(device.socket_fd) + "]");
    }
}

bool DeviceRegistry::is_already_connected(std::string device_id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    for (const auto& device : devices) {
        if (device.device_id == device_id && device.state != DeviceState::DISCONNECTED) {
            return true;
        }
    }
    return false;
}

std::vector<Device> DeviceRegistry::get_devices_copy() {
    std::lock_guard<std::mutex> lock(registry_mutex);
    return devices;
}

}
