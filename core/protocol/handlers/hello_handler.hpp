#pragma once

// Hello packet handler, basically registers device.
#include "../packet.hpp"
#include <memory>
namespace gasoline {
class Connection;
class HelloHandler {
public:
    enum class Action {
        Continue,
        DisconnectCurrent,
        DisconnectPeer
    };

    struct Result {
        Action action = Action::Continue;
        std::shared_ptr<Connection> peer_connection;
    };

    static Result handle(const Packet& pkt, const std::shared_ptr<Connection>& connection);
};

}
