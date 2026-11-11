#include "ping_handler.hpp"

#include "../../networking/send_packet.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/device_id.hpp"

namespace gasoline {
void PingHandler::handle(const Packet& pkt, const std::shared_ptr<Connection>& connection) {
    nlohmann::json response;
    response["type"] = "pong";
    response["device_id"] = get_my_device_id();
    send_packet(connection, response);
    log("Pong sent");
}

}
