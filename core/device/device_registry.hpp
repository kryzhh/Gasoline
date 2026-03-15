#pragma once

#include <vector>
#include <mutex> // Using mutexes to ensure no race conditions since server is multithreaded
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

private:

    std::vector<Device> devices;
    std::mutex registry_mutex;

};

extern DeviceRegistry device_registry;

}