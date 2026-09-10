#include "device_registry.hpp"
#include "../utils/logger.hpp"
#include "../networking/connection.hpp"

#include <algorithm>

namespace gasoline {

DeviceRegistry device_registry;

DeviceRegistry::RegistrationResult DeviceRegistry::add_device(const Device& device) {
    // Release strong references after the registry lock: their destructors may
    // perform session cleanup, which also acquires this lock.
    auto candidate = device.connection.lock();
    std::shared_ptr<Connection> owner;
    std::lock_guard<std::mutex> lock(registry_mutex);
    if (!candidate || candidate->is_stopping()) {
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
        RegistrationResult result{true, existing};
        existing = device;
        return result;
    }
    devices.push_back(device);
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

bool DeviceRegistry::set_state_for_session(uint64_t session_id, DeviceState state) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    for (auto& device : devices) {
        if (device.session_id == session_id) {
            device.state = state;
            return true;
        }
    }
    return false;
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
