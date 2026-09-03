#include "device_id.hpp"

#include "../identity/device_identity.hpp"

namespace gasoline {

std::string get_my_device_id() {
    static const DeviceIdentity identity = DeviceIdentity::load_or_create();
    return identity.device_id();

}

}