#include "connection.hpp"

#include "connection_manager.hpp"
#include "../device/device_registry.hpp"
#include "../utils/device_id.hpp"
#include "../protocol/packet.hpp"
#include "../protocol/router/packet_router.hpp"
#include "../utils/logger.hpp"
#include "../utils/packet_monitor.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <thread>

namespace gasoline {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto HANDSHAKE_TIMEOUT = std::chrono::seconds(10);
constexpr auto WRITE_TIMEOUT = std::chrono::seconds(10);
std::atomic<uint64_t> next_session_id{1};

class BoundedPacketBuffer : public std::streambuf {
public:
    BoundedPacketBuffer(std::string& data, size_t limit) : data_(data), limit_(limit) {
        data_.reserve(limit + 1); // Includes space for the framing newline.
    }

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (static_cast<size_t>(count) > limit_ - data_.size()) {
            throw std::length_error("packet exceeds maximum frame size");
        }
        data_.append(data, static_cast<size_t>(count));
        return count;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        const char character = traits_type::to_char_type(value);
        xsputn(&character, 1);
        return value;
    }

private:
    std::string& data_;
    const size_t limit_;
};

int wait_for_socket(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        int timeout = -1;
        if (deadline != Clock::time_point::max()) {
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) {
                return 0;
            }
            const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            timeout = static_cast<int>(std::min<int64_t>(milliseconds, INT_MAX));
        }
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, timeout);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result;
    }
}

} // namespace

std::shared_ptr<Connection> Connection::create(int socket_fd, Role role) {
    auto connection = std::shared_ptr<Connection>(new Connection(socket_fd, role));
    ConnectionManager::instance().register_connection(connection);
    return connection;
}

Connection::Connection(int socket_fd, Role role)
    : socket_fd_(socket_fd), session_id_(next_session_id.fetch_add(1)), role_(role),
      handshake_deadline_(Clock::now() + HANDSHAKE_TIMEOUT) {}

Connection::~Connection() {
    finalize_disconnect("connection released");
}

int Connection::socket_fd() const { return socket_fd_; }
uint64_t Connection::session_id() const { return session_id_; }
bool Connection::is_outgoing() const { return role_ == Role::Outgoing; }
bool Connection::is_stopping() const { return stopping_.load(); }

bool Connection::mark_ready() {
    if (stopping_.load() || (!ready_.load() && Clock::now() >= handshake_deadline_) ||
        !device_registry.set_state_for_session(session_id_, DeviceState::READY)) {
        return false;
    }
    ready_.store(true);
    return true;
}

void Connection::send_hello() {
    nlohmann::json pkt;
    pkt["type"] = "hello";
    pkt["device_id"] = get_my_device_id();
    pkt["payload"]["device_name"] = "Gasoline";
    pkt["payload"]["device_type"] = "linux";
    if (send_packet(pkt) < 0) {
        request_disconnect("hello send failed");
    }
}

void Connection::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }
    try {
        // The worker owns a strong reference until receive and cleanup finish.
        std::thread([self = shared_from_this()]() {
            try {
                self->send_hello();
                self->receive_loop();
            } catch (const std::exception& e) {
                log(std::string("Session worker failed: ") + e.what());
            } catch (...) {
                log("Session worker failed");
            }
            self->finalize_disconnect("receive worker ended");
        }).detach();
    } catch (const std::exception& e) {
        log(std::string("Failed to start session worker: ") + e.what());
        finalize_disconnect("worker startup failed");
    }
}

void Connection::request_disconnect(const std::string& reason) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (stopping_.exchange(true)) {
        return;
    }
    log("Session stopping " + std::to_string(session_id_) + ": " + reason);
    // Shutdown may interrupt a writer, but close waits for that writer to exit.
    ::shutdown(socket_fd_, SHUT_RDWR);
}

ssize_t Connection::send_packet(const nlohmann::json& packet) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stopping_.load()) {
        return -1;
    }
    std::string data;
    BoundedPacketBuffer buffer(data, MAX_FRAME_SIZE);
    std::ostream output(&buffer);
    // Stop serialization on the first overflow; never send a partial frame.
    output.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        output << packet;
    } catch (const std::length_error&) {
        return -1;
    }
    data.push_back('\n');
    log_tx(std::to_string(session_id_), packet.value("type", "unknown"));
    const ssize_t result = send_all(data);
    if (result < 0) {
        request_disconnect("send failed or timed out");
    }
    return result;
}

ssize_t Connection::send_all(const std::string& data) {
    const auto deadline = ready_.load() ? Clock::now() + WRITE_TIMEOUT :
        std::min(Clock::now() + WRITE_TIMEOUT, handshake_deadline_);
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        if (stopping_.load() || Clock::now() >= deadline) {
            return -1;
        }
        const ssize_t sent = ::send(socket_fd_, data.data() + total_sent,
                                    data.size() - total_sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                wait_for_socket(socket_fd_, POLLOUT, deadline) > 0) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        total_sent += static_cast<size_t>(sent);
    }
    return static_cast<ssize_t>(total_sent);
}

void Connection::finalize_disconnect(const std::string& reason) {
    request_disconnect(reason);
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    std::lock_guard<std::mutex> socket_lock(socket_mutex_);
    if (cleaned_up_.exchange(true)) {
        return;
    }
    device_registry.remove_device(session_id_);
    ConnectionManager::instance().unregister_connection(session_id_);
    if (::close(socket_fd_) != 0) {
        log(std::string("Close failed: ") + std::strerror(errno));
    }
}

bool Connection::handle_frame() {
    if (stopping_.load()) {
        return false;
    }
    if (!ready_.load() && Clock::now() >= handshake_deadline_) {
        request_disconnect("handshake timeout");
        return false;
    }
    try {
        const Packet pkt = parse_packet(incoming_buffer_);
        if ((pkt.type == "hello" && peer_hello_received_) ||
            (pkt.type != "hello" && !peer_hello_received_) ||
            (peer_hello_received_ && pkt.device_id != peer_device_id_)) {
            request_disconnect("unexpected packet or changed peer identity");
            return false;
        }
        const auto result = PacketRouter::route(pkt, shared_from_this());
        if (result.peer_connection) {
            result.peer_connection->request_disconnect("duplicate connection replaced");
        }
        if (result.action == PacketRouteAction::Disconnect) {
            request_disconnect("protocol requested disconnect");
            return false;
        }
        if (pkt.type == "hello") {
            peer_device_id_ = pkt.device_id;
            peer_hello_received_ = true;
        }
    } catch (const std::exception& e) {
        log(std::string("Invalid packet: ") + e.what());
        request_disconnect("invalid packet");
        return false;
    }
    return !stopping_.load();
}

void Connection::receive_loop() {
    char buffer[4096];
    while (!stopping_.load()) {
        const auto deadline = ready_.load() ? Clock::time_point::max() : handshake_deadline_;
        const int result = wait_for_socket(socket_fd_, POLLIN, deadline);
        if (result <= 0) {
            request_disconnect(result == 0 ? "handshake timeout" : "receive poll failed");
            break;
        }
        const ssize_t bytes = ::recv(socket_fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (bytes < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (bytes <= 0) {
            request_disconnect(bytes == 0 ? "peer closed connection" : "receive error");
            break;
        }

        // Bound each frame before appending, including frames whose delimiter
        // arrives in this read. Coalesced frames each receive their own limit.
        std::string_view chunk(buffer, static_cast<size_t>(bytes));
        while (!chunk.empty() && !stopping_.load()) {
            const auto newline = chunk.find('\n');
            const auto length = newline == std::string_view::npos ? chunk.size() : newline;
            if (length > MAX_FRAME_SIZE - incoming_buffer_.size()) {
                request_disconnect("oversized frame");
                break;
            }
            incoming_buffer_.append(chunk.data(), length);
            if (newline == std::string_view::npos) {
                break;
            }
            if (!handle_frame()) {
                break;
            }
            incoming_buffer_.clear();
            chunk.remove_prefix(length + 1);
        }
    }
}

} // namespace gasoline
