#include "send_packet.hpp"
#include "connection.hpp"
#include "connection_manager.hpp"
#include "../utils/packet_monitor.hpp"
#include "../utils/logger.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>

namespace gasoline {

namespace {

ssize_t send_all_raw(int socket_fd, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        const ssize_t sent = ::send(socket_fd, data.data() + total_sent, data.size() - total_sent, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            log(std::string("Send failed on socket ") + std::to_string(socket_fd) + ": " + std::strerror(errno));
            return -1;
        }

        if (sent == 0) {
            log("Send returned zero on socket: " + std::to_string(socket_fd));
            return -1;
        }

        total_sent += static_cast<size_t>(sent);
    }

    return static_cast<ssize_t>(total_sent);
}

} // namespace

ssize_t send_packet(const std::shared_ptr<Connection>& connection, const nlohmann::json& packet) {
    if (!connection) {
        return -1;
    }

    return connection->send_packet(packet);
}

ssize_t send_packet(int socket_fd, const nlohmann::json& packet) {

    auto connection = ConnectionManager::instance().find(socket_fd);
    if (connection) {
        return connection->send_packet(packet);
    }

    const std::string data = packet.dump() + "\n";
    log_tx(std::to_string(socket_fd), packet["type"]);
    return send_all_raw(socket_fd, data);
}

}