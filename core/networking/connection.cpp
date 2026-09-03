#include "connection.hpp"

#include "connection_manager.hpp"
#include "../device/device_registry.hpp"
#include "../utils/device_id.hpp"
#include "../protocol/packet.hpp"
#include "../protocol/router/packet_router.hpp"
#include "../utils/logger.hpp"
#include "../utils/packet_monitor.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>

namespace gasoline {

namespace {

bool is_receive_error_retryable(int error_code) {
    return error_code == EINTR;
}

} // namespace

std::shared_ptr<Connection> Connection::create(int socket_fd, Role role) {
    auto connection = std::shared_ptr<Connection>(new Connection(socket_fd, role));
    ConnectionManager::instance().register_connection(connection);
    return connection;
}

Connection::Connection(int socket_fd, Role role)
    : socket_fd_(socket_fd), role_(role) {}

int Connection::socket_fd() const {
    return socket_fd_;
}

bool Connection::is_outgoing() const {
    return role_ == Role::Outgoing;
}

void Connection::send_hello() {
    nlohmann::json pkt;
    pkt["type"] = "hello";
    pkt["device_id"] = get_my_device_id();
    pkt["payload"]["device_name"] = "Gasoline";
    pkt["payload"]["device_type"] = "linux";

    if (send_packet(pkt) < 0) {
        log("Failed to send hello packet");
        request_disconnect("hello send failed");
        return;
    }

    log("Hello packet sent");
}

void Connection::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    auto self = shared_from_this();
    std::thread([self]() {
        self->receive_loop();
    }).detach();

    log("Session started for socket: " + std::to_string(socket_fd_));
    send_hello();
}

void Connection::request_disconnect(const std::string& reason) {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) {
        return;
    }

    if (!reason.empty()) {
        log("Session stopping for socket " + std::to_string(socket_fd_) + ": " + reason);
    } else {
        log("Session stopping for socket " + std::to_string(socket_fd_));
    }

    ::shutdown(socket_fd_, SHUT_RDWR);
}

ssize_t Connection::send_packet_locked(const nlohmann::json& packet) {
    const std::string data = packet.dump() + "\n";
    return send_all(socket_fd_, data);
}

ssize_t Connection::send_packet(const nlohmann::json& packet) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    log_tx(std::to_string(socket_fd_), packet.value("type", "unknown"));
    const ssize_t result = send_packet_locked(packet);
    if (result < 0) {
        request_disconnect("send failed");
    }
    return result;
}

ssize_t Connection::send_all(int socket_fd, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        const ssize_t sent = ::send(socket_fd, data.data() + total_sent, data.size() - total_sent, 0);
        if (sent < 0) {
            if (is_receive_error_retryable(errno)) {
                continue;
            }
            log(std::string("Send failed on socket ") + std::to_string(socket_fd) + ": " + std::strerror(errno));
            return -1;
        }

        if (sent == 0) {
            log("Send returned zero on socket: " + std::to_string(socket_fd));
            return -1;
        }

        total_sent += static_cast<size_t>(sent);
    }

    return static_cast<ssize_t>(total_sent);
}

void Connection::finalize_disconnect(const std::string& reason) {
    bool expected = false;
    if (!cleaned_up_.compare_exchange_strong(expected, true)) {
        return;
    }

    if (!reason.empty()) {
        log("Session closed for socket " + std::to_string(socket_fd_) + ": " + reason);
    } else {
        log("Session closed for socket " + std::to_string(socket_fd_));
    }

    device_registry.set_state_for_socket(socket_fd_, DeviceState::DISCONNECTED);
    device_registry.remove_device(socket_fd_);
    ConnectionManager::instance().unregister_connection(socket_fd_);

    if (::close(socket_fd_) != 0) {
        log(std::string("Close failed on socket ") + std::to_string(socket_fd_) + ": " + std::strerror(errno));
    }
}

void Connection::receive_loop() {
    log("Receive loop active for socket: " + std::to_string(socket_fd_));

    char buffer[4096];
    while (!stopping_.load()) {
        const ssize_t bytes = ::recv(socket_fd_, buffer, sizeof(buffer), 0);
        if (bytes == 0) {
            log("Peer disconnected normally on socket: " + std::to_string(socket_fd_));
            request_disconnect("peer closed connection");
            break;
        }

        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }

            log(std::string("Receive error on socket ") + std::to_string(socket_fd_) + ": " + std::strerror(errno));
            request_disconnect("receive error");
            break;
        }

        incoming_buffer_.append(buffer, static_cast<size_t>(bytes));

        size_t newline_position = std::string::npos;
        while ((newline_position = incoming_buffer_.find('\n')) != std::string::npos) {
            const std::string packet_str = incoming_buffer_.substr(0, newline_position);
            incoming_buffer_.erase(0, newline_position + 1);

            try {
                Packet pkt = parse_packet(packet_str);
                log("Packet received on socket " + std::to_string(socket_fd_) + ": " + pkt.type);
                const auto route_result = PacketRouter::route(pkt, socket_fd_);
                if (route_result.action == PacketRouteAction::DisconnectPeer && route_result.peer_socket_fd >= 0) {
                    if (auto peer_connection = ConnectionManager::instance().find(route_result.peer_socket_fd)) {
                        peer_connection->request_disconnect("duplicate connection replaced");
                    }
                }
                if (route_result.action == PacketRouteAction::Disconnect) {
                    request_disconnect("protocol requested disconnect");
                    break;
                }
            } catch (const std::exception& e) {
                log(std::string("Invalid packet on socket ") + std::to_string(socket_fd_) + ": " + e.what());
            }
        }

        if (stopping_.load()) {
            break;
        }

        if (incoming_buffer_.size() > MAX_FRAME_SIZE) {
            log("Incoming frame exceeded maximum size on socket: " + std::to_string(socket_fd_));
            request_disconnect("oversized frame");
            break;
        }
    }

    finalize_disconnect(stopping_.load() ? "stopped" : "receive loop ended");
}

} // namespace gasoline