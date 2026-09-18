#include "packet_router.hpp"
#include "../../device/device_registry.hpp"
#include "../../networking/connection.hpp"
#include "../../utils/logger.hpp"
#include "../../networking/send_packet.hpp"
#include "../../utils/packet_monitor.hpp"

// Handlers
#include "../handlers/hello_handler.hpp"
#include "../handlers/ping_handler.hpp"
#include "../handlers/message_handler.hpp"

namespace gasoline {

PacketRouteResult PacketRouter::route(const Packet& pkt,
                                      const std::shared_ptr<Connection>& connection,
                                      const AuthorizedPeer* authenticated_peer) {
    PacketRouteResult result;
    log_rx(authenticated_peer == nullptr ? std::to_string(connection->session_id()) :
                                          authenticated_peer->device_id(),
           pkt.type);
    log("Routing packet type: " + pkt.type);
    if (pkt.type == "hello") {
        const auto hello_result = HelloHandler::handle(pkt, connection);
        if (hello_result.action == HelloHandler::Action::DisconnectCurrent) {
            result.action = PacketRouteAction::Disconnect;
            return result;
        }
        if (hello_result.action == HelloHandler::Action::DisconnectPeer) {
            result.action = PacketRouteAction::DisconnectPeer;
            result.peer_connection = hello_result.peer_connection;
            return result;
        }
        return result;
    }
    if (pkt.type == "ping") {
        PingHandler::handle(pkt, connection);
        return result;
    }
    if (pkt.type == "pong") {
        return result;
    }
    if (pkt.type == "message") {
        if (authenticated_peer == nullptr) {
            result.action = PacketRouteAction::Disconnect;
            return result;
        }
        MessageHandler::handle(pkt, *authenticated_peer);
        return result;
    }
    log("Unknown packet type: " + pkt.type);
    return result;
}

} // namespace gasoline
