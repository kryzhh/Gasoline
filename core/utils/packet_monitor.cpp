#include "packet_monitor.hpp"
#include <iostream>

namespace gasoline {
void log_rx(const std::string& device_id, const std::string& type) {
    std::cout << "[RX] " << device_id << " -> " << type << std::endl;
}

void log_tx(const std::string& device_id, const std::string& type) {
    std::cout << "[TX] " << device_id << " <- " << type << std::endl;
}

}