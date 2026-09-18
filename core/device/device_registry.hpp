#pragma once

#include <vector>
#include <mutex> // Using mutexes to ensure no race conditions since server is multithreaded
#include <optional>

#include "device.hpp"

namespace gasoline { class AuthorizedPeer; }

namespace gasoline {

/*
    Class for managing devices
    This is where we add and remove devices
*/    

class DeviceRegistry { 

public:

    struct RegistrationResult {
        bool accepted = false;
        std::optional<Device> replaced_device;
    };

    void remove_device(uint64_t session_id);
    void list_devices();
    bool is_already_connected(std::string device_id);
    std::vector<Device> get_devices_copy();

private:
    friend class Connection;
    // Final validation, duplicate arbitration, READY transition, and ownership
    // replacement are committed together under registry_mutex.
    RegistrationResult publish_authenticated_device(
        const Device& device, const AuthorizedPeer& authorization);

    std::vector<Device> devices;
    std::mutex registry_mutex;

};

extern DeviceRegistry device_registry;

}
