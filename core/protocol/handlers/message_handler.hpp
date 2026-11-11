#pragma once

#include "../packet.hpp"

/*
    Handles normal packets
    Processes normal messages, extract texts etc.
*/

namespace gasoline {

class AuthorizedPeer;

class MessageHandler {
public:
    static void handle(const Packet& pkt, const AuthorizedPeer& authenticated_peer);
};

}
