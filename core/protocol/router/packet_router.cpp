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

PacketRouteAction PacketRouter::route(const Packet& pkt, int socket_fd) {
    log_rx(pkt.device_id, pkt.type);
    log("Routing packet type: " + pkt.type);
    if (pkt.type == "hello") {
        return HelloHandler::handle(pkt, socket_fd) == HelloHandler::Action::Disconnect
                   ? PacketRouteAction::Disconnect
                   : PacketRouteAction::Continue;
    }
    if (pkt.type == "ping") {
        device_registry.set_state_for_socket(socket_fd, DeviceState::READY);
        PingHandler::handle(pkt, socket_fd);
        return PacketRouteAction::Continue;
    }
    if (pkt.type == "pong") {
        device_registry.set_state_for_socket(socket_fd, DeviceState::READY);
        log("Connection stabilized; device marked READY");
        return PacketRouteAction::Continue;
    }
    if (pkt.type == "message") {
        MessageHandler::handle(pkt, socket_fd);
        return PacketRouteAction::Continue;
    }
    log("Unknown packet type: " + pkt.type);
    return PacketRouteAction::Continue;
}

}