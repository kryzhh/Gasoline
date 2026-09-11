#pragma once
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

// Defining packet structure

namespace gasoline {

struct Packet {

    std::string type;
    std::string device_id;

    nlohmann::json payload; // Using actual json parser now

};

Packet parse_packet(std::string_view data);

// Serializes only the protocol payload. Transport framing is handled separately.
std::string serialize_packet(const Packet& pkt);

}
