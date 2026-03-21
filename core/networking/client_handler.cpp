// Includes
#include "client_handler.hpp"
#include "../utils/logger.hpp"
#include "../protocol/packet.hpp"
#include "../../include/gasoline/config.hpp"
#include "../device/device_registry.hpp"
#include "../device/device.hpp"
#include "../protocol/router/packet_router.hpp"

// Sockets
#include <unistd.h>
#include <sys/socket.h>

namespace gasoline {

ClientHandler::ClientHandler(int socket_fd) { 
    this->socket_fd = socket_fd; // Stores the socket inside the object
}

// To create instances (devices), since program multithreaded, each handle called is a new device.
void ClientHandler::handle() {

    char buffer[BUFFER_SIZE];
    std::string incoming_buffer; // ✅ MOVED OUTSIDE LOOP

    log("Handler started for socket: " + std::to_string(socket_fd));

    while (true) {
        int bytes = recv(socket_fd, buffer, BUFFER_SIZE, 0);

        if (bytes == 0) {
            log("Client disconnected normally");
            break;
        }

        if (bytes < 0) {
            log("Socket error during recv()");
            break;
        }

        incoming_buffer.append(buffer, bytes);

        size_t pos;
        while ((pos = incoming_buffer.find('\n')) != std::string::npos) { // Building a packet framing system, as we can have situations where we can have packets in fragment. We use \n to end current packet

            std::string packet_str = incoming_buffer.substr(0, pos);
            incoming_buffer.erase(0, pos + 1);

            try {
                Packet pkt = parse_packet(packet_str);
                log("Packet received: " + pkt.type);
                PacketRouter::route(pkt, socket_fd);
            }
            catch (const std::exception& e) {
                log(std::string("Invalid packet received: ") + e.what());
            }
        }
    }

    device_registry.remove_device(socket_fd);
    close(socket_fd);
    log("Device disconnected");
    device_registry.list_devices();
}

}