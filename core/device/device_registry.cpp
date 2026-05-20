#include "device_registry.hpp"
#include "../utils/logger.hpp"
#include "../protocol/packet.hpp"
#include "../networking/send_packet.hpp"
#include "../utils/device_id.hpp"

#include <algorithm>

namespace gasoline {

DeviceRegistry device_registry;

void DeviceRegistry::add_device(const Device& device) { // Adds device to the registry based on the socket it is connected to
    std::lock_guard<std::mutex> lock(registry_mutex); // Ensure simultaenous access of registry (vector containing devices) happens in a safe manner 
    devices.push_back(device);
    log("Device registered: " + device.device_name);
}

void DeviceRegistry::remove_device(int socket_fd) { // Removes device from the registry based on socket it is connected to
    std::lock_guard<std::mutex> lock(registry_mutex);
    devices.erase(
        std::remove_if(devices.begin(), devices.end(), // Removes only if device actually exists
                       [socket_fd](const Device& d) {
                           return d.socket_fd == socket_fd;
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

void DeviceRegistry::send_to_device(const std::string& device_id, const nlohmann::json& packet) {
    for (auto& device : devices) {
        if (device.device_id == device_id) {
            send_packet(device.socket_fd, packet);
            return;
        }
    }
    log("Device not found: " + device_id);
}

void DeviceRegistry::broadcast(const nlohmann::json& packet) {
    for (auto& device : devices) {
        if (device.socket_fd <= 0)
            continue;
        if (!device.ready)
            continue;
        if (device.device_id == get_my_device_id())
            continue;
        log("Sending to READY device: " + device.device_id);

        send_packet(device.socket_fd, packet);
    }
}


const std::vector<Device>& DeviceRegistry::get_devices() const {
    return devices;
}

}