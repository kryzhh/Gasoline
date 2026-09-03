/*
    Right now this is a linux only server, will make a Windows server soon enough.
    Want to have atleast one platform work.
*/

// Inclusion of all header files made (common stuff)
#include "server.hpp"
#include "../utils/logger.hpp"
#include "../../include/gasoline/config.hpp"
#include "connection.hpp"

// Server exclusive stuff
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace gasoline {

void Server::start() { // Start function declaration

    int server_fd; // File descriptor 
    sockaddr_in address{}; // Stores IPv4 Info (Initialized to 0)

    server_fd = socket(AF_INET, SOCK_STREAM, 0); // Creates a new socket.
    if (server_fd < 0) {
        log(std::string("Failed to create server socket: ") + std::strerror(errno));
        return;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        log(std::string("Failed to set socket options: ") + std::strerror(errno));
        close(server_fd);
        return;
    }

    address.sin_family = AF_INET; // Tells we are using IPv4
    address.sin_addr.s_addr = INADDR_ANY; // Accept connection from all interfaces
    address.sin_port = htons(SERVER_PORT); 

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        log(std::string("Failed to bind server socket: ") + std::strerror(errno));
        close(server_fd);
        return;
    }

    if (listen(server_fd, 5) != 0) {
        log(std::string("Failed to listen on server socket: ") + std::strerror(errno));
        close(server_fd);
        return;
    }

    log("Listening on port " + std::to_string(SERVER_PORT));

    while (true) {
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);
        const int client_socket = accept(server_fd,
                                (struct sockaddr*)&client_address,
                                &client_length);
        if (client_socket < 0) {
            if (errno == EINTR) {
                continue;
            }
            log(std::string("Accept failed: ") + std::strerror(errno));
            continue;
        }

        log("Device connected on socket: " + std::to_string(client_socket));

        auto connection = Connection::create(client_socket);
        connection->start();
    }
}

}