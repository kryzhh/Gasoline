/*
    Right now this is a linux only server, will make a Windows server soon enough.
    Want to have atleast one platform work.
*/

// Inclusion of all header files made (common stuff)
#include "server.hpp"
#include "../utils/logger.hpp"
#include "../protocol/packet.hpp"
#include "../../include/gasoline/config.hpp"

// Server exclusive stuff
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

namespace gasoline {

void Server::start() { // Start function declaration

    int server_fd, client_socket; // File descriptors 
    sockaddr_in address{}; // Stores IPv4 Info (Initialized to 0)
    int addrlen = sizeof(address); 

    server_fd = socket(AF_INET, SOCK_STREAM, 0); // Creates a new socket.

    address.sin_family = AF_INET; // Tells we are using IPv4
    address.sin_addr.s_addr = INADDR_ANY; // Accept connection from all interfaces
    address.sin_port = htons(SERVER_PORT); 

    bind(server_fd, (struct sockaddr*)&address, sizeof(address)); // Binding port to address and given port
    listen(server_fd, 5); // Up to 5 connections can be pending (basically a wait queue)

    log("Listening on port " + std::to_string(SERVER_PORT));

    while (true) {

        client_socket = accept(server_fd,
                               (struct sockaddr*)&address,
                               (socklen_t*)&addrlen);

        log("Device connected");

        char buffer[BUFFER_SIZE] = {0}; // Empty buffer initialized

        int bytes = recv(client_socket, buffer, BUFFER_SIZE, 0); // Read data from client socket

        if (bytes > 0) {
            std::string data(buffer);
            Packet pkt = parse_packet(data); // Parse data received
            log("Packet received: " + pkt.type); // Logs kind of data received
        }

        close(client_socket); // Closes connection once data received
    }
}

}