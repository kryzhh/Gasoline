#pragma once
#include "../packet.hpp"

// Implementing Packet router 
/*
    Why do we even need a packet router you ask, seperation of responsibilities really.
    Don't wanna make the client handler too big else will be a pain to work with later (yes gpt suggested it).
    Here (well, in packet_router.cpp) I can define how each type of packet is handled without cluttering client_handler
*/

namespace gasoline {
class PacketRouter {

public:
    static void route(const Packet& pkt, int socket_fd);

private:
    static void handle_hello(const Packet& pkt, int socket_fd);

};

}