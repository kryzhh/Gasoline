#include "ping_handler.hpp"

#include "../../networking/send_packet.hpp"
#include "../../utils/logger.hpp"

namespace gasoline {
void PingHandler::handle(const Packet& pkt, int socket_fd) {
    nlohmann::json response;
    response["type"] = "pong";
    send_packet(socket_fd, response);
    log("Pong sent");
}

}