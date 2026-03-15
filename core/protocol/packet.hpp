#pragma once
#include <string>

// Defining packet structure

namespace gasoline {

struct Packet {
    std::string type; // Tells what kind of packet is coming, for eg: ping, pong, notification etc
    std::string raw; // The entire message (in JSON but as string, no parsing yet)
};

Packet parse_packet(const std::string& data);

}