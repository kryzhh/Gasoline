#include "send_packet.hpp"
#include "../utils/packet_monitor.hpp"

#include <sys/socket.h>
#include <string>

namespace gasoline {

void send_packet(int socket_fd, const nlohmann::json& packet) {

    std::string data = packet.dump();
    data += "\n";   // Adding the delimeter to adhere to framing standards
    log_tx("server", packet["type"]);
    send(socket_fd, data.c_str(), data.size(), 0);
}

}