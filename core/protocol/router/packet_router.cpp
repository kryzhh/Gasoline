#include "packet_router.hpp"
#include "../../device/device_registry.hpp"
#include "../../device/device.hpp"
#include "../../utils/logger.hpp"
#include "../../networking/send_packet.hpp"
#include "../../utils/packet_monitor.hpp"

#include <sys/socket.h>

// Handlers
#include "../handlers/hello_handler.hpp"
#include "../handlers/ping_handler.hpp"
#include "../handlers/message_handler.hpp"

namespace gasoline {

void PacketRouter::route(const Packet& pkt, int socket_fd) {
    log_rx(pkt.device_id, pkt.type);
    log("Routing packet type: " + pkt.type);
    if (pkt.type == "hello") {
        HelloHandler::handle(pkt, socket_fd);
        return;
    }
    if (pkt.type == "ping") {
        device_registry.set_state_for_socket(socket_fd, DeviceState::READY);
        PingHandler::handle(pkt, socket_fd);
        return;
    }
    if (pkt.type == "pong") {
        device_registry.set_state_for_socket(socket_fd, DeviceState::READY);
        log("Connection stabilized; device marked READY");
        return;
    }
    if (pkt.type == "message") {
        MessageHandler::handle(pkt, socket_fd);
        return;
    }
    else log("Unknown packet type: " + pkt.type);
}

void PacketRouter::handle_hello(const Packet& pkt, int socket_fd) {
    Device device;

    device.device_id = pkt.device_id;
    device.device_name = pkt.payload["device_name"];
    device.device_type = pkt.payload["device_type"];
    device.socket_fd = socket_fd;
    device_registry.add_device(device);

    device_registry.list_devices();
}

void PacketRouter::handle_ping(const Packet& pkt, int socket_fd) {
    nlohmann::json response;
    response["type"] = "pong";
    send_packet(socket_fd, response);
    log("Pong sent");
}

}