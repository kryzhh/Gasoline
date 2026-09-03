#include "hello_handler.hpp"

#include "../../device/device_registry.hpp"
#include "../../device/device.hpp"
#include "../../networking/connection.hpp"
#include "../../networking/connection_manager.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/device_id.hpp"
#include "../../networking/send_packet.hpp"

#include <optional>

namespace gasoline {

HelloHandler::Result HelloHandler::handle(const Packet& pkt, int socket_fd) {

    Result result;

    std::string my_id = get_my_device_id();
    std::string peer_id = pkt.device_id;

    log("My ID: " + my_id + " | Peer ID: " + peer_id);

    if (peer_id.empty() || my_id.empty()) {
        log("Missing device_id in hello packet");
        result.action = Action::DisconnectCurrent;
        return result;
    }

    if (my_id == peer_id) {
        log("Peer is self; rejecting connection");
        result.action = Action::DisconnectCurrent;
        return result;
    }

    const auto connection = ConnectionManager::instance().find(socket_fd);
    const bool is_outgoing = connection ? connection->is_outgoing() : false;

    Device device;
    device.device_id = peer_id;
    device.device_name = pkt.payload["device_name"];
    device.device_type = pkt.payload["device_type"];
    device.socket_fd = socket_fd;
    device.state = DeviceState::HANDSHAKE_DONE;

    const auto replaced_device = device_registry.add_device(device);

    if (!replaced_device.has_value()) {

        nlohmann::json ping;
        ping["type"] = "ping";
        ping["device_id"] = my_id;
        send_packet(socket_fd, ping);

        log("Device registered: " + device.device_id);
        return result;
    }

    const bool local_is_lower = my_id < peer_id;
    const bool current_should_keep = (local_is_lower == is_outgoing);

    if (!current_should_keep) {
        log("Duplicate connection rejected by UUID ownership rule");
        device_registry.add_device(*replaced_device);
        result.action = Action::DisconnectCurrent;
        return result;
    }

    log("Duplicate connection accepted by UUID ownership rule; replacing existing socket " + std::to_string(replaced_device->socket_fd));

    nlohmann::json ping;
    ping["type"] = "ping";
    ping["device_id"] = my_id;
    send_packet(socket_fd, ping);

    log("Device registered: " + device.device_id);
    result.action = Action::DisconnectPeer;
    result.peer_socket_fd = replaced_device->socket_fd;
    return result;
}

}