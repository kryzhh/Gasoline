#include "packet_router.hpp"
#include "../../device/device_registry.hpp"
#include "../../device/device.hpp"
#include "../../utils/logger.hpp"

namespace gasoline {

void PacketRouter::route(const Packet& pkt, int socket_fd) {
    if (pkt.type == "hello") {
        handle_hello(pkt, socket_fd);
        return;
    }
    log("Unknown packet type: " + pkt.type);
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

}