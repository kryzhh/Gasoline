#include "packet.hpp"

// Function declartion for packet parsing

namespace gasoline {

Packet parse_packet(const std::string& data) {

    Packet pkt;
    pkt.raw = data;

    // Validation of packet received, it should fall withing given categories, for now only 2 categories
    // ping and notification, will add more rn just setting up basic daemon.

    if (data.find("\"type\":\"ping\"") != std::string::npos)
        pkt.type = "ping";
    else if (data.find("\"type\":\"notification\"") != std::string::npos)
        pkt.type = "notification";
    else
        pkt.type = "unknown";

    return pkt;
}

}