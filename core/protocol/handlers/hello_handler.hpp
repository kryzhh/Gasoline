#pragma once

// Hello packet handler, basically registers device.
#include "../packet.hpp"
namespace gasoline {
class HelloHandler {
public:
    enum class Action {
        Continue,
        DisconnectCurrent,
        DisconnectPeer
    };

    struct Result {
        Action action = Action::Continue;
        int peer_socket_fd = -1;
    };

    static Result handle(const Packet& pkt, int socket_fd);
};

}