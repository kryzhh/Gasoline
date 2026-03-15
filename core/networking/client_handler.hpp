#pragma once

// Defining the client (Aka the phone) since right now the server opens a new
// connection for each packet and closes it, we need a persistent connection 
// (initial goal was to only setup a daemon)

namespace gasoline {

class ClientHandler {
    public:
        ClientHandler(int socket_fd); // Each client connect to it's own socket
        void handle();

    private:
        int socket_fd;
    };

}