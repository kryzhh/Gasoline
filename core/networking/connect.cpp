#include "connect.hpp"
#include "../utils/logger.hpp"
#include "connection.hpp"

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

    auto connection = Connection::create(sock, Connection::Role::Outgoing);
    connection->start();
}

}