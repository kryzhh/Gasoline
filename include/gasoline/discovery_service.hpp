#pragma once

/*
    Discovery service, for automatic discovery of server to client.
    Advertise the Gasoline daemon on the local network using mDNS.
    Create a service named "_gasoline._tcp.local". Devices query who this? and server gives IP + Port
    Using avahi for Linux, will see what to use for Windows when we get there.
*/
namespace gasoline {

class DiscoveryService {
public:
    void start();
};

}