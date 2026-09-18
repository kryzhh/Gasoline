#include "connection.hpp"

#include "connection_manager.hpp"
#include "../device/device_registry.hpp"
#include "../utils/device_id.hpp"
#include "../protocol/packet.hpp"
#include "../protocol/v2_framing.hpp"
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
        data_.reserve(limit);
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

bool is_control_record(const nlohmann::json& packet) {
    const auto type = packet.find("type");
    if (type == packet.end() || !type->is_string()) {
        return false;
    }
    const auto& value = type->get_ref<const std::string&>();
    return value == "hello" || value == "ping" || value == "pong";
}

bool is_control_record(const Packet& packet) {
    return packet.type == "hello" || packet.type == "ping" || packet.type == "pong";
}

} // namespace

std::shared_ptr<Connection> Connection::create(
    int socket_fd, Role role, std::shared_ptr<SessionAuthentication> authentication) {
    if (!authentication) {
        ::close(socket_fd);
        throw std::invalid_argument("Connection requires SessionAuthentication");
    }
    auto connection = std::shared_ptr<Connection>(
        new Connection(socket_fd, role, std::move(authentication)));
    ConnectionManager::instance().register_connection(connection);
    return connection;
}

Connection::Connection(int socket_fd, Role role,
                       std::shared_ptr<SessionAuthentication> authentication)
    : socket_fd_(socket_fd), session_id_(next_session_id.fetch_add(1)), role_(role),
      authentication_(std::move(authentication)),
      handshake_deadline_(Clock::now() + HANDSHAKE_TIMEOUT) {}

Connection::~Connection() {
    finalize_disconnect("connection released");
}

int Connection::socket_fd() const { return socket_fd_; }
uint64_t Connection::session_id() const { return session_id_; }
bool Connection::is_outgoing() const { return role_ == Role::Outgoing; }
bool Connection::is_stopping() const { return stopping_.load(); }
bool Connection::is_ready() const { return ready_.load(); }

std::optional<AuthorizedPeer> Connection::authorize_verified_identity(
    const VerifiedPeerIdentity& identity) const {
    if (!authentication_ || stopping_.load() || ready_.load() ||
        Clock::now() >= handshake_deadline_) {
        return std::nullopt;
    }

    auto authorization = authentication_->authorize_verified_peer(identity);
    if (!authorization || authorization->device_id() == get_my_device_id()) {
        return std::nullopt;
    }
    return authorization;
}

bool Connection::complete_authentication_after_verified_identity(
    const VerifiedPeerIdentity& identity,
    std::string device_name, std::string device_type) {
    auto authorization = authorize_verified_identity(identity);
    if (!authorization) {
        return false;
    }
    return activate_authorized_peer(std::move(*authorization),
                                    std::move(device_name), std::move(device_type));
}

bool Connection::activate_authorized_peer(
    AuthorizedPeer authorization, std::string device_name, std::string device_type) {
    if (stopping_.load() || ready_.load() || Clock::now() >= handshake_deadline_) {
        return false;
    }

    Device device;
    device.device_id = authorization.device_id();
    device.device_name = std::move(device_name);
    device.device_type = std::move(device_type);
    device.socket_fd = socket_fd_;
    device.session_id = session_id_;
    device.connection = shared_from_this();
    device.preferred_connection =
        (get_my_device_id() < device.device_id) == is_outgoing();
    device.state = DeviceState::AUTHENTICATED;
    device.public_key = authorization.public_key();
    device.trust_revision = authorization.revision();
    device.permissions = authorization.permissions();

    DeviceRegistry::RegistrationResult registration;
    bool live_authorization_registered = false;
    {
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (stopping_.load() || ready_.load() || Clock::now() >= handshake_deadline_) {
            return false;
        }
        const std::weak_ptr<Connection> weak_self = weak_from_this();
        live_authorization_registered = authentication_->register_live_session(
            session_id_, authorization, [weak_self] {
                if (const auto connection = weak_self.lock()) {
                    connection->request_disconnect("live trust authorization invalidated");
                }
            });
        if (!live_authorization_registered) {
            return false;
        }
        try {
            registration = device_registry.publish_authenticated_device(device, authorization);
        } catch (...) {
            authentication_->unregister_live_session(session_id_);
            throw;
        }
        if (!registration.accepted) {
            authentication_->unregister_live_session(session_id_);
            return false;
        }
        authorized_peer_ = std::move(authorization);
        ready_.store(true);
    }
    if (registration.replaced_device) {
        if (auto replaced = registration.replaced_device->connection.lock()) {
            replaced->request_disconnect("authenticated duplicate connection replaced");
        }
    }
    return true;
}

#ifdef GASOLINE_SESSION_TESTING
bool Connection::complete_authentication_for_test(
    const VerifiedPeerIdentity& identity,
    std::string device_name, std::string device_type,
    std::function<void()> after_authorization) {
    auto authorization = authorize_verified_identity(identity);
    if (!authorization) {
        return false;
    }
    if (after_authorization) {
        after_authorization();
    }
    return activate_authorized_peer(std::move(*authorization),
                                    std::move(device_name), std::move(device_type));
}
#endif

ssize_t Connection::send_initial_hello() {
    nlohmann::json pkt;
    pkt["type"] = "hello";
    pkt["device_id"] = get_my_device_id();
    pkt["payload"]["device_name"] = "Gasoline";
    pkt["payload"]["device_type"] = "linux";

    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stopping_.load() || !local_preface_sent_) {
        return -1;
    }
    const ssize_t result = send_packet_locked(pkt);
    return result;
}

ssize_t Connection::send_preface() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stopping_.load()) {
        return -1;
    }
    if (local_preface_sent_) {
        return 0;
    }
    const auto preface = protocol_v2::preface_bytes();
    const ssize_t result = send_all(std::string(preface.data(), preface.size()));
    if (result < 0) {
        request_disconnect("protocol v2 preface send failed or timed out");
        return -1;
    }
    local_preface_sent_ = true;
    return result;
}

bool Connection::accept_peer_preface() {
    bool expected = false;
    if (peer_preface_received_.compare_exchange_strong(expected, true)) {
        if (send_initial_hello() < 0) {
            request_disconnect("hello send failed");
        }
    }
    return !stopping_.load();
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
                if (self->send_preface() >= 0) {
                    self->receive_loop();
                }
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
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
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
    if (stopping_.load() || !ready_.load() || !authorized_peer_) {
        return -1;
    }
    if (!is_control_record(packet)) {
        if (!authentication_->authorization_is_current(*authorized_peer_)) {
            request_disconnect("stale authorization before application send");
            return -1;
        }
    }
    return send_packet_locked(packet);
}

ssize_t Connection::send_packet_locked(const nlohmann::json& packet) {
    std::string data;
    const size_t limit = is_control_record(packet) ? MAX_CONTROL_RECORD_SIZE :
                                                     protocol_v2::MAX_RECORD_SIZE;
    BoundedPacketBuffer buffer(data, limit);
    std::ostream output(&buffer);
    // Stop serialization on the first overflow; never send a partial frame.
    output.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        output << packet;
    } catch (const std::length_error&) {
        return -1;
    }
    std::string wire_record = protocol_v2::encode_record(data);
    log_tx(std::to_string(session_id_), packet.value("type", "unknown"));
    const ssize_t result = send_all(wire_record);
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
    authentication_->unregister_live_session(session_id_);
    device_registry.remove_device(session_id_);
    ConnectionManager::instance().unregister_connection(session_id_);
    if (::close(socket_fd_) != 0) {
        log(std::string("Close failed: ") + std::strerror(errno));
    }
}

bool Connection::handle_record(std::string_view payload) {
    if (stopping_.load()) {
        return false;
    }
    if (!ready_.load() && Clock::now() >= handshake_deadline_) {
        request_disconnect("handshake timeout");
        return false;
    }
    if (!ready_.load() && !peer_hello_received_ && payload.size() > MAX_CONTROL_RECORD_SIZE) {
        request_disconnect("oversized setup record");
        return false;
    }
    try {
        const Packet pkt = parse_packet(payload);
        if (is_control_record(pkt) && payload.size() > MAX_CONTROL_RECORD_SIZE) {
            request_disconnect("oversized control record");
            return false;
        }
        if (!ready_.load()) {
            if (pkt.type != "hello" || peer_hello_received_) {
                request_disconnect("application/control packet before authentication");
                return false;
            }
            const auto result = PacketRouter::route(pkt, shared_from_this(), nullptr);
            if (result.action == PacketRouteAction::Disconnect) {
                request_disconnect("invalid unverified hello");
                return false;
            }
            peer_hello_received_ = true;
            return !stopping_.load();
        }
        if (pkt.type == "hello" || !authorized_peer_) {
            request_disconnect("unexpected setup packet after authentication");
            return false;
        }
        if (!is_control_record(pkt)) {
            if (!authentication_->authorization_is_current(*authorized_peer_)) {
                request_disconnect("stale authorization before application receive");
                return false;
            }
        }
        const auto result = PacketRouter::route(
            pkt, shared_from_this(), &*authorized_peer_);
        if (result.action == PacketRouteAction::Disconnect) {
            request_disconnect("protocol requested disconnect");
            return false;
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
        if (bytes < 0) {
            request_disconnect("receive error");
            break;
        }
        if (bytes == 0) {
            try {
                record_parser_.finish();
                request_disconnect("peer closed connection");
            } catch (const protocol_v2::FramingError& e) {
                log(std::string("Invalid protocol v2 EOF: ") + e.what());
                request_disconnect("truncated protocol v2 stream");
            }
            break;
        }

        try {
            record_parser_.consume(
                std::string_view(buffer, static_cast<size_t>(bytes)),
                [this](std::string_view payload) {
                    return accept_peer_preface() && handle_record(payload);
                });
            if (record_parser_.preface_complete() && !stopping_.load()) {
                accept_peer_preface();
            }
        } catch (const protocol_v2::FramingError& e) {
            log(std::string("Invalid protocol v2 framing: ") + e.what());
            request_disconnect("invalid protocol v2 framing");
        }
    }
}

} // namespace gasoline
