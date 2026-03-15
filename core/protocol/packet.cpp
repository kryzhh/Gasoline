#include "packet.hpp"
using json = nlohmann::json;

// Function declartion for packet parsing

namespace gasoline {

Packet parse_packet(const std::string& data) {
    json j = json::parse(data); // Parse the JSON received
    Packet pkt; // Create the packet
    // And add the values

    pkt.type = j["type"];
    pkt.device_id = j["device_id"];
    if (j.contains("payload"))
        pkt.payload = j["payload"];

    return pkt;
}

}