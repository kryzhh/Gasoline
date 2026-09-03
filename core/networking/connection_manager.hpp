#pragma once

#include <memory>

namespace gasoline {

class Connection;

class ConnectionManager {
public:
    static ConnectionManager& instance();

    void register_connection(const std::shared_ptr<Connection>& connection);
    std::shared_ptr<Connection> find(int socket_fd);
    void unregister_connection(int socket_fd);

private:
    ConnectionManager() = default;
};

} // namespace gasoline