// Remember to enjoy the light before it's gone
 
#include "../core/networking/server.hpp" // Include server
#include <gasoline/discovery_service.hpp> // Include discovery service
#include "../core/discovery/discovery_client.hpp"

#include <signal.h>
#include <thread>

using namespace gasoline; 

// Main function of the program
int main() {
    signal(SIGPIPE, SIG_IGN); // Ensures daemon doesn't crash if client disconnects while sending data
    
    Server server;
    DiscoveryClient discovery;
    DiscoveryService discovery_service; 

    // Run server in a thread
    std::thread server_thread([&]() {
        server.start();
    });

    // Start mDNS advertisement
    std::thread discovery_service_thread([&]() {
        discovery_service.start();
    });

    // Start discovery (blocking)
    discovery.start();

    server_thread.join();
    discovery_service_thread.join();

    server_thread.join();

    return 0;
}