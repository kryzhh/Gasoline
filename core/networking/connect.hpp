#pragma once
#include <memory>
#include <string>

/*
    How the devices will connect? 
    Defines auto connect and registrations
*/

namespace gasoline {

class SessionAuthentication;

void connect_to_device(const std::string& ip, int port,
                       std::shared_ptr<SessionAuthentication> authentication);

}
