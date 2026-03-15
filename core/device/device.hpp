#pragma once
#include <string>

// Uniquely storing info of each device
namespace gasoline {

struct Device {

    std::string device_id;
    std::string device_name;
    std::string device_type;

    int socket_fd;

};

}