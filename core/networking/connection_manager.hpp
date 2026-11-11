#pragma once

#include <memory>
#include <cstdint>

namespace gasoline {

class Connection;

class ConnectionManager {
public:
    static ConnectionManager& instance();

    void register_connection(const std::shared_ptr<Connection>& connection);
    std::shared_ptr<Connection> find(uint64_t session_id);
    void unregister_connection(uint64_t session_id);

private:
    ConnectionManager() = default;
};

} // namespace gasoline
