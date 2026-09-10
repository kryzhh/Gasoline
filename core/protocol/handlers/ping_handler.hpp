#pragma once

#include "../packet.hpp"
#include <memory>

namespace gasoline {
class Connection;
class PingHandler { // Ping handler
public:
    static void handle(const Packet& pkt, const std::shared_ptr<Connection>& connection);
};

}
