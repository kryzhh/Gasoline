// Includes
#include "client_handler.hpp"
#include "../utils/logger.hpp"
#include "connection.hpp"

namespace gasoline {

ClientHandler::ClientHandler(int socket_fd, std::shared_ptr<SessionAuthentication> authentication)
    : socket_fd(socket_fd), authentication_(std::move(authentication)) {}

void ClientHandler::handle() {
    auto connection = Connection::create(
        socket_fd, Connection::Role::Incoming, authentication_);
    connection->start();
}

}
