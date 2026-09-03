#include "hello_handler.hpp"

#include "../../device/device_registry.hpp"
#include "../../device/device.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/device_id.hpp"
#include "../../networking/send_packet.hpp"

#include <unistd.h>

namespace gasoline {

HelloHandler::Action HelloHandler::handle(const Packet& pkt, int socket_fd) {

    std::string my_id = get_my_device_id();
    std::string peer_id = pkt.device_id;

    log("My ID: " + my_id + " | Peer ID: " + peer_id);

    // Connection Ownership rule
    if (peer_id.empty() || my_id.empty()) {
        log("Invalid device_id format");
        return Action::Disconnect;
    }

    if (my_id == peer_id || my_id > peer_id) {
        log("Closing connection (peer should initiate)");
        return Action::Disconnect;
    }

    Device device;

    device.device_id = peer_id;
    device.device_name = pkt.payload["device_name"];
    device.device_type = pkt.payload["device_type"];
    device.socket_fd = socket_fd;
    device.state = DeviceState::HANDSHAKE_DONE;
    device_registry.add_device(device);

    nlohmann::json ping;
    ping["type"] = "ping";
    ping["device_id"] = my_id;
    send_packet(socket_fd, ping);

    log("Device registered: " + device.device_id);
    return Action::Continue;
}

}