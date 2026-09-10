#include "hello_handler.hpp"

#include "../../device/device_registry.hpp"
#include "../../device/device.hpp"
#include "../../networking/connection.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/device_id.hpp"
#include "../../networking/send_packet.hpp"

#include <optional>

namespace gasoline {

HelloHandler::Result HelloHandler::handle(const Packet& pkt, const std::shared_ptr<Connection>& connection) {

    Result result;

    if (pkt.type != "hello" || !pkt.payload.is_object()) {
        log("Invalid hello type or payload");
        result.action = Action::DisconnectCurrent;
        return result;
    }
    const auto device_name = pkt.payload.find("device_name");
    const auto device_type = pkt.payload.find("device_type");
    if (device_name == pkt.payload.end() || !device_name->is_string() ||
        device_type == pkt.payload.end() || !device_type->is_string()) {
        log("Hello device_name and device_type must be present and strings");
        result.action = Action::DisconnectCurrent;
        return result;
    }

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

    Device device;
    device.device_id = peer_id;
    device.device_name = device_name->get<std::string>();
    device.device_type = device_type->get<std::string>();
    device.socket_fd = connection->socket_fd();
    device.session_id = connection->session_id();
    device.connection = connection;
    device.preferred_connection = (my_id < peer_id) == connection->is_outgoing();
    device.state = DeviceState::HANDSHAKE_DONE;

    const auto registration = device_registry.add_device(device);
    if (!registration.accepted) {
        log("Duplicate connection rejected by UUID ownership rule");
        result.action = Action::DisconnectCurrent;
        return result;
    }

    if (registration.replaced_device) {
        result.action = Action::DisconnectPeer;
        result.peer_connection = registration.replaced_device->connection.lock();
    }

    nlohmann::json ping;
    ping["type"] = "ping";
    ping["device_id"] = my_id;
    send_packet(connection, ping);

    log("Device registered: " + device.device_id);
    return result;
}

}
