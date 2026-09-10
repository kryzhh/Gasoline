#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>

#include <nlohmann/json.hpp>

namespace gasoline {

class Connection : public std::enable_shared_from_this<Connection> {
public:
    enum class Role {
        Incoming,
        Outgoing
    };

    static std::shared_ptr<Connection> create(int socket_fd, Role role);
    ~Connection();

    int socket_fd() const;
    uint64_t session_id() const;
    bool is_outgoing() const;
    bool is_stopping() const;
    bool mark_ready();

    void start();
    void request_disconnect(const std::string& reason);
    ssize_t send_packet(const nlohmann::json& packet);

private:
    explicit Connection(int socket_fd, Role role);

    void send_hello();
    void receive_loop();
    bool handle_frame();
    void finalize_disconnect(const std::string& reason);
    ssize_t send_all(const std::string& data);

    static constexpr size_t MAX_FRAME_SIZE = 65536;

    const int socket_fd_;
    const uint64_t session_id_;
    Role role_;
    const std::chrono::steady_clock::time_point handshake_deadline_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> cleaned_up_{false};
    std::atomic<bool> ready_{false};
    std::mutex write_mutex_;
    std::mutex socket_mutex_;
    bool peer_hello_received_{false};
    std::string peer_device_id_;
    std::string incoming_buffer_;
};

} // namespace gasoline
