// Remember to enjoy the light before it's gone
 
#include "../core/networking/server.hpp" // Include server
#include <gasoline/discovery_service.hpp> // Include discovery service

#include <signal.h>

// Main function of the program
int main() {
    signal(SIGPIPE, SIG_IGN); // Ensures daemon doesn't crash if client disconnects while sending data

    gasoline::DiscoveryService discovery;
    discovery.start();

    gasoline::Server server;
    server.start();

}