// Remember to enjoy the light before it's gone
#include "../core/networking/server.hpp"
#include <gasoline/discovery_service.hpp>
#include "../core/discovery/discovery_client.hpp"
#include "../core/utils/device_id.hpp"
#include "../core/device/device_registry.hpp"
#include "../core/utils/logger.hpp"

#include <nlohmann/json.hpp>
#include <signal.h>
#include <thread>
#include <chrono>

using namespace gasoline;

int main() {
    signal(SIGPIPE, SIG_IGN); // Prevent crash on broken pipe

    Server server;
    DiscoveryClient discovery;
    DiscoveryService discovery_service;

    // Thread 1: TCP Server
    std::thread server_thread([&]() {
        server.start();
    });

    // Thread 2: mDNS Advertisement (Avahi service)
    std::thread discovery_service_thread([&]() {
        discovery_service.start();
    });

    // Thread 3: Send test message AFTER connection
    std::thread sender_thread([&]() {
        while (true) {
            auto& devices = device_registry.get_devices();
            bool found_ready = false;
            for (const auto& d : devices) {
                if (d.ready && d.device_id != get_my_device_id()) {
                    found_ready = true;
                    break;
                }
            }
            if (found_ready)
                break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::this_thread::sleep_for(std::chrono::seconds(1)); // small buffer
        nlohmann::json pkt;
        pkt["type"] = "message";
        pkt["device_id"] = get_my_device_id();
        pkt["payload"]["text"] = "Hello from my system";

        log("Sending test message...");
        device_registry.broadcast(pkt);
    });

    // Main thread: Discovery (BLOCKING)
    discovery.start();

    // Cleanup (won’t really reach here unless stopped)
    server_thread.join();
    discovery_service_thread.join();
    sender_thread.join();

    return 0;
}