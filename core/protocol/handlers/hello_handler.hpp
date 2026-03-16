#pragma once

// Hello packet handler, basically registers device.
#include "../packet.hpp"
namespace gasoline {
class HelloHandler {
public:
    static void handle(const Packet& pkt, int socket_fd);
};

}