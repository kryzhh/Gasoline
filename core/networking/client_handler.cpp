// Includes
#include "client_handler.hpp"
#include "../utils/logger.hpp"
#include "connection.hpp"

namespace gasoline {

ClientHandler::ClientHandler(int socket_fd) { 
    this->socket_fd = socket_fd; // Stores the socket inside the object
}

void ClientHandler::handle() {
    auto connection = Connection::create(socket_fd);
    connection->start();
}

}