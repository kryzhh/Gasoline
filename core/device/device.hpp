#pragma once
#include <string>
#include <cstdint>
#include <memory>
#include <vector>

#include "../trust/trust_store.hpp"

// Uniquely storing info of each device
namespace gasoline {

class Connection;

enum class DeviceState { // Device states
    CONNECTING,
    HANDSHAKE_DONE, // Legacy display state; unverified sessions are never registered.
    AUTHENTICATED,
    READY,
    DISCONNECTED
};

struct Device {

    std::string device_id;
    std::string device_name;
    std::string device_type;

    int socket_fd = -1; // Diagnostic only; never used to look up or send to a session.
    uint64_t session_id = 0;
    std::weak_ptr<Connection> connection;
    bool preferred_connection = false;
    DeviceState state = DeviceState::CONNECTING;
    TrustPublicKey public_key{};
    int64_t trust_revision = 0;
    std::vector<std::string> permissions;

};

}
