#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>

#include <nlohmann/json.hpp>

#include "../protocol/v2_framing.hpp"
#include "../auth/session_authentication.hpp"

namespace gasoline {

class Connection : public std::enable_shared_from_this<Connection> {
public:
    enum class Role {
        Incoming,
        Outgoing
    };

    static std::shared_ptr<Connection> create(
        int socket_fd, Role role, std::shared_ptr<SessionAuthentication> authentication);
    ~Connection();

    int socket_fd() const;
    uint64_t session_id() const;
    bool is_outgoing() const;
    bool is_stopping() const;
    bool is_ready() const;

    void start();
    void request_disconnect(const std::string& reason);
    ssize_t send_packet(const nlohmann::json& packet);

#ifdef GASOLINE_SESSION_TESTING
    // The production build has no public authentication-completion entry point.
    // Session tests use this to exercise post-authentication lifecycle behavior
    // until the cryptographic handshake owns the private completion path.
    bool complete_authentication_for_test(
        const VerifiedPeerIdentity& identity,
        std::string device_name = "Test peer",
        std::string device_type = "test",
        std::function<void()> after_authorization = {});
#endif

private:
    explicit Connection(int socket_fd, Role role,
                        std::shared_ptr<SessionAuthentication> authentication);

    ssize_t send_preface();
    bool accept_peer_preface();
    ssize_t send_initial_hello();
    ssize_t send_packet_locked(const nlohmann::json& packet);
    void receive_loop();
    bool handle_record(std::string_view payload);
    bool complete_authentication_after_verified_identity(
        const VerifiedPeerIdentity& identity,
        std::string device_name, std::string device_type);
    std::optional<AuthorizedPeer> authorize_verified_identity(
        const VerifiedPeerIdentity& identity) const;
    bool activate_authorized_peer(AuthorizedPeer authorization,
                                  std::string device_name, std::string device_type);
    void finalize_disconnect(const std::string& reason);
    ssize_t send_all(const std::string& data);

    static constexpr size_t MAX_CONTROL_RECORD_SIZE = 4096;

    const int socket_fd_;
    const uint64_t session_id_;
    Role role_;
    std::shared_ptr<SessionAuthentication> authentication_;
    const std::chrono::steady_clock::time_point handshake_deadline_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> cleaned_up_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> peer_preface_received_{false};
    std::mutex write_mutex_;
    // Serializes the final READY/registry commit against cancellation. Lock
    // order, when combined, is write_mutex_ -> lifecycle_mutex_ -> socket_mutex_.
    std::mutex lifecycle_mutex_;
    std::mutex socket_mutex_;
    bool local_preface_sent_{false}; // Guarded by write_mutex_.
    bool peer_hello_received_{false};
    std::optional<AuthorizedPeer> authorized_peer_;
    protocol_v2::RecordParser record_parser_;
};

} // namespace gasoline
