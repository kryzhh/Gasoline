#include "connection_manager.hpp"

#include "connection.hpp"

#include <mutex>
#include <unordered_map>

namespace gasoline {

namespace {

std::mutex manager_mutex;
std::unordered_map<int, std::weak_ptr<Connection>> connections;

} // namespace

ConnectionManager& ConnectionManager::instance() {
    static ConnectionManager manager;
    return manager;
}

void ConnectionManager::register_connection(const std::shared_ptr<Connection>& connection) {
    std::lock_guard<std::mutex> lock(manager_mutex);
    connections[connection->socket_fd()] = connection;
}

std::shared_ptr<Connection> ConnectionManager::find(int socket_fd) {
    std::lock_guard<std::mutex> lock(manager_mutex);
    const auto iterator = connections.find(socket_fd);
    if (iterator == connections.end()) {
        return nullptr;
    }

    auto connection = iterator->second.lock();
    if (!connection) {
        connections.erase(iterator);
    }
    return connection;
}

void ConnectionManager::unregister_connection(int socket_fd) {
    std::lock_guard<std::mutex> lock(manager_mutex);
    connections.erase(socket_fd);
}

} // namespace gasoline