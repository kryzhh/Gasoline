#include "connect.hpp"
#include "../utils/logger.hpp"
#include "../protocol/packet.hpp"
#include "../../include/gasoline/config.hpp"
#include "../utils/device_id.hpp"

#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>

namespace gasoline {

void connect_to_device(const std::string& ip, int port) {

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        log("Failed to create socket");
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        log("Invalid IP address");
        close(sock);
        return;
    }

    log("Connecting to " + ip + ":" + std::to_string(port));

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log("Connection failed");
        close(sock);
        return;
    }

    log("Connected to device");

    // Send hello packet
    Packet pkt;
    pkt.type = "hello";
    pkt.device_id = get_my_device_id();
    pkt.payload["device_name"] = "Gasoline";
    pkt.payload["device_type"] = "linux";

    std::string data = serialize_packet(pkt);

    send(sock, data.c_str(), data.size(), 0);

    log("Hello packet sent");

    // For now: close after sending
    // close(sock);
}

}