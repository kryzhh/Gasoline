/*
    Right now this is a linux only server, will make a Windows server soon enough.
    Want to have atleast one platform work.
*/

// Inclusion of all header files made (common stuff)
#include "server.hpp"
#include "../utils/logger.hpp"
#include "../protocol/packet.hpp"
#include "../../include/gasoline/config.hpp"
#include "client_handler.hpp"

// Server exclusive stuff
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

// Multithreading
#include <thread> 

namespace gasoline {

void Server::start() { // Start function declaration

    int server_fd, client_socket; // File descriptors 
    sockaddr_in address{}; // Stores IPv4 Info (Initialized to 0)
    int addrlen = sizeof(address); 

    server_fd = socket(AF_INET, SOCK_STREAM, 0); // Creates a new socket.
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // Failsafe to ensure server is restarted instantly
    // If this not added, OS would keep the port occupied for a cooldown period, which will give issues if server is to be restarted
    // After some update or daemon restarts

    address.sin_family = AF_INET; // Tells we are using IPv4
    address.sin_addr.s_addr = INADDR_ANY; // Accept connection from all interfaces
    address.sin_port = htons(SERVER_PORT); 

    bind(server_fd, (struct sockaddr*)&address, sizeof(address)); // Binding port to address and given port
    listen(server_fd, 5); // Up to 5 connections can be pending (basically a wait queue)

    log("Listening on port " + std::to_string(SERVER_PORT));

    while (true) {
        int client_socket = accept(server_fd,
                                (struct sockaddr*)&address,
                                (socklen_t*)&addrlen);
        log("Device connected");

        std::thread client_thread([client_socket]() { // New thread for each device connected
            gasoline::ClientHandler handler(client_socket);
            handler.handle();

        });

        client_thread.detach();
    }
}

}