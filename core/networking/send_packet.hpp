#pragma once

#include <nlohmann/json.hpp>
#include <sys/types.h>
#include <memory>

/*
    Creating a helper function to send packets, else will have to do basic operations like adding delimeter (\n) everytime manually
*/

namespace gasoline {

ssize_t send_packet(int socket_fd, const nlohmann::json& packet);
ssize_t send_packet(const std::shared_ptr<class Connection>& connection, const nlohmann::json& packet);

}