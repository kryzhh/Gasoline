#include "device_id.hpp"

#include "../identity/device_identity.hpp"

namespace gasoline {

const DeviceIdentity& get_my_device_identity() {
    static const DeviceIdentity identity = DeviceIdentity::load_or_create();
    return identity;
}

std::string get_my_device_id() {
    return get_my_device_identity().device_id();
}

}
