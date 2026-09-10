#include "packet.hpp"
#include <stdexcept>
using json = nlohmann::json;

// Function declartion for packet parsing

namespace gasoline {

Packet parse_packet(const std::string& data) {
    const json j = json::parse(data);
    if (!j.is_object()) {
        throw std::invalid_argument("packet must be an object");
    }
    const auto type = j.find("type");
    const auto device_id = j.find("device_id");
    if (type == j.end() || !type->is_string() ||
        device_id == j.end() || !device_id->is_string()) {
        throw std::invalid_argument("packet type and device_id must be present and strings");
    }
    Packet pkt; // Create the packet
    // And add the values

    pkt.type = type->get<std::string>();
    pkt.device_id = device_id->get<std::string>();
    const auto payload = j.find("payload");
    if (pkt.type == "hello" && (payload == j.end() || !payload->is_object())) {
        throw std::invalid_argument("hello payload must be present and an object");
    }
    if (payload != j.end())
        pkt.payload = *payload;

    return pkt;
}

std::string serialize_packet(const Packet& pkt) {

    json j;

    j["type"] = pkt.type;
    j["device_id"] = pkt.device_id;
    j["payload"] = pkt.payload;

    return j.dump() + "\n";
}

}
