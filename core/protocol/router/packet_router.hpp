#pragma once
#include "../packet.hpp"
#include <memory>

// Implementing Packet router 
/*
    Why do we even need a packet router you ask, seperation of responsibilities really.
    Don't wanna make the client handler too big else will be a pain to work with later (yes gpt suggested it).
    Here (well, in packet_router.cpp) I can define how each type of packet is handled without cluttering client_handler
*/

namespace gasoline {

class Connection;

enum class PacketRouteAction {
    Continue,
    Disconnect,
    DisconnectPeer
};

struct PacketRouteResult {
    PacketRouteAction action = PacketRouteAction::Continue;
    std::shared_ptr<Connection> peer_connection;
};

class PacketRouter {

public:
    static PacketRouteResult route(const Packet& pkt, const std::shared_ptr<Connection>& connection);

};

}
