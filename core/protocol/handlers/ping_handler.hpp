#pragma once

#include "../packet.hpp"

namespace gasoline {
class PingHandler { // Ping handler
public:
    static void handle(const Packet& pkt, int socket_fd);
};

}