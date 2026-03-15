#pragma once
#include <string>
#include <nlohmann/json.hpp>

// Defining packet structure

namespace gasoline {

struct Packet {

    std::string type;
    std::string device_id;

    nlohmann::json payload; // Using actual json parser now

};

Packet parse_packet(const std::string& data);

}