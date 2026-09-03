#pragma once

// Compatibility wrapper around the shared connection/session path.

namespace gasoline {

class ClientHandler {
    public:
        explicit ClientHandler(int socket_fd);
        void handle();

    private:
        int socket_fd;
    };

}