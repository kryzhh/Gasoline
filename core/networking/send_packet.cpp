#include "send_packet.hpp"
#include "../utils/packet_monitor.hpp"
#include "../utils/logger.hpp"

#include <sys/socket.h>
#include <string>

namespace gasoline {

ssize_t send_packet(int socket_fd, const nlohmann::json& packet) {

    std::string data = packet.dump();
    data += "\n";   // Adding the delimeter to adhere to framing standards
    log_tx("server", packet["type"]);
    ssize_t sent = send(socket_fd, data.c_str(), data.size(), 0);

    if (sent < 0) {
        log("Send failed on socket: " + std::to_string(socket_fd));
    }

    return sent;
}

}