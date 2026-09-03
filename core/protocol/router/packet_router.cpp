#include "packet_router.hpp"
#include "../../device/device_registry.hpp"
#include "../../utils/logger.hpp"
#include "../../networking/send_packet.hpp"
#include "../../utils/packet_monitor.hpp"

// Handlers
#include "../handlers/hello_handler.hpp"
#include "../handlers/ping_handler.hpp"
#include "../handlers/message_handler.hpp"

namespace gasoline {

PacketRouteResult PacketRouter::route(const Packet& pkt, int socket_fd) {
    PacketRouteResult result;
    log_rx(pkt.device_id, pkt.type);
    log("Routing packet type: " + pkt.type);
    if (pkt.type == "hello") {
        const auto hello_result = HelloHandler::handle(pkt, socket_fd);
        if (hello_result.action == HelloHandler::Action::DisconnectCurrent) {
            result.action = PacketRouteAction::Disconnect;
            return result;
        }
        if (hello_result.action == HelloHandler::Action::DisconnectPeer) {
            result.action = PacketRouteAction::DisconnectPeer;
            result.peer_socket_fd = hello_result.peer_socket_fd;
            return result;
        }
        return result;
    }
    if (pkt.type == "ping") {
        device_registry.set_state_for_socket(socket_fd, DeviceState::READY);
        PingHandler::handle(pkt, socket_fd);
        return result;
    }
    if (pkt.type == "pong") {
        device_registry.set_state_for_socket(socket_fd, DeviceState::READY);
        log("Connection stabilized; device marked READY");
        return result;
    }
    if (pkt.type == "message") {
        MessageHandler::handle(pkt, socket_fd);
        return result;
    }
    log("Unknown packet type: " + pkt.type);
    return result;
}

} // namespace gasoline
