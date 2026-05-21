#include "message_service.hpp"

#include "../core/device/device_registry.hpp"
#include "../core/device/device.hpp"
#include "../core/networking/send_packet.hpp"
#include "../core/utils/logger.hpp"
#include "../core/utils/device_id.hpp"

#include <nlohmann/json.hpp>
#include <vector>

namespace gasoline {

namespace {
nlohmann::json build_message_packet(const std::string& text) {
    nlohmann::json pkt;
    pkt["type"] = "message";
    pkt["device_id"] = get_my_device_id();
    pkt["payload"]["text"] = text;
    return pkt;
}
}

void MessageService::send_to_device(const std::string& device_id, const std::string& text) {
    if (text.empty()) {
        log("Empty message, skipping");
        return;
    }

    const auto pkt = build_message_packet(text);
    const auto devices = device_registry.get_devices_copy();

    for (const auto& device : devices) {
        if (device.device_id != device_id)
            continue;
        if (device.socket_fd <= 0) {
            log("Cannot send to invalid socket for device: " + device_id);
            return;
        }
        if (device.state != DeviceState::READY) {
            log("Device is not ready for messages: " + device_id);
            return;
        }

        const ssize_t sent = send_packet(device.socket_fd, pkt);
        if (sent < 0) {
            log("Failed to send message to: " + device.device_id);
        }
        return;
    }

    log("Device not found: " + device_id);
}

void MessageService::broadcast_message(const std::string& text) {
    if (text.empty()) {
        log("Empty message, skipping");
        return;
    }

    const auto pkt = build_message_packet(text);
    const auto devices = device_registry.get_devices_copy();

    for (const auto& device : devices) {
        if (device.socket_fd <= 0)
            continue;
        if (device.state != DeviceState::READY)
            continue;
        if (device.device_id == get_my_device_id())
            continue;

        log("Sending to device: " + device.device_id);
        const ssize_t sent = send_packet(device.socket_fd, pkt);
        if (sent < 0) {
            log("Failed to send message to: " + device.device_id);
        }
    }
}

}
