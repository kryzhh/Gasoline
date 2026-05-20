#pragma once

#include "../packet.hpp"

/*
    Handles normal packets
    Processes normal messages, extract texts etc.
*/

namespace gasoline {

class MessageHandler {
public:
    static void handle(const Packet& pkt, int socket_fd);
};

}