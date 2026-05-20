#include "hello_handler.hpp"

#include "../../device/device_registry.hpp"
#include "../../device/device.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/device_id.hpp"

#include <unistd.h>

namespace gasoline {

void HelloHandler::handle(const Packet& pkt, int socket_fd) {

    std::string my_id = get_my_device_id();
    std::string peer_id = pkt.device_id;

    log("My ID: " + my_id + " | Peer ID: " + peer_id);

    // Connection Ownership rule
    if (std::stoull(my_id) > std::stoull(peer_id)) {
        log("Closing connection (peer should initiate)");

        close(socket_fd);
        device_registry.remove_device(socket_fd);
        return;
    }

    Device device;

    device.device_id = peer_id;
    device.device_name = pkt.payload["device_name"];
    device.device_type = pkt.payload["device_type"];
    device.socket_fd = socket_fd;
    device.is_connected = true;

    device.ready = true;
    device_registry.add_device(device);

    log("Device registered: " + device.device_id);
}

}