#pragma once

#include <memory>

// Class declaration of server
// Function prototype for start function of server

namespace gasoline {

class SessionAuthentication;

class Server {
public:
    explicit Server(std::shared_ptr<SessionAuthentication> authentication);
    void start();

private:
    std::shared_ptr<SessionAuthentication> authentication_;
};

}
