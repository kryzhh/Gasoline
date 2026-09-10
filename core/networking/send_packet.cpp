#include "send_packet.hpp"
#include "connection.hpp"

namespace gasoline {

ssize_t send_packet(const std::shared_ptr<Connection>& connection, const nlohmann::json& packet) {
    if (!connection) {
        return -1;
    }

    return connection->send_packet(packet);
}

}
