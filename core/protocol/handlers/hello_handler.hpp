#pragma once

// Hello packet handler, basically registers device.
#include "../packet.hpp"
namespace gasoline {
class HelloHandler {
public:
    enum class Action {
        Continue,
        Disconnect
    };

    static Action handle(const Packet& pkt, int socket_fd);
};

}