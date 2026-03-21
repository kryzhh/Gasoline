#pragma once
#include <string>

/*
    How the devices will connect? 
    Defines auto connect and registrations
*/

namespace gasoline {

void connect_to_device(const std::string& ip, int port);

}