#pragma once

#include <vector>
#include <mutex> // Using mutexes to ensure no race conditions since server is multithreaded
#include <nlohmann/json.hpp>

#include "device.hpp"

namespace gasoline {

/*
    Class for managing devices
    This is where we add and remove devices
*/    

class DeviceRegistry { 

public:

    void add_device(const Device& device);
    void remove_device(int socket_fd);
    void list_devices();
    bool is_already_connected(std::string device_id);
    void send_to_device(const std::string& device_id, const nlohmann::json& packet);
    void broadcast(const nlohmann::json& packet);
    const std::vector<Device>& get_devices() const;

private:

    std::vector<Device> devices;
    std::mutex registry_mutex;

};

extern DeviceRegistry device_registry;

}