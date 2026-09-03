#include "ping_handler.hpp"

#include "../../networking/send_packet.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/device_id.hpp"

namespace gasoline {
void PingHandler::handle(const Packet& pkt, int socket_fd) {
    nlohmann::json response;
    response["type"] = "pong";
    response["device_id"] = get_my_device_id();
    send_packet(socket_fd, response);
    log("Pong sent");
}

}