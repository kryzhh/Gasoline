#pragma once

#include <memory>

// Compatibility wrapper around the shared connection/session path.

namespace gasoline {

class SessionAuthentication;

class ClientHandler {
    public:
        ClientHandler(int socket_fd, std::shared_ptr<SessionAuthentication> authentication);
        void handle();

    private:
        int socket_fd;
        std::shared_ptr<SessionAuthentication> authentication_;
    };

}
