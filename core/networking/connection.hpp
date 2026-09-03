#pragma once

#include <atomic>
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

    int socket_fd() const;
    bool is_outgoing() const;

    void start();
    void request_disconnect(const std::string& reason);
    ssize_t send_packet(const nlohmann::json& packet);

private:
    explicit Connection(int socket_fd, Role role);

    void send_hello();
    void receive_loop();
    void finalize_disconnect(const std::string& reason);
    ssize_t send_packet_locked(const nlohmann::json& packet);
    static ssize_t send_all(int socket_fd, const std::string& data);

    static constexpr size_t MAX_FRAME_SIZE = 65536;

    int socket_fd_;
    Role role_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> cleaned_up_{false};
    std::mutex write_mutex_;
    std::string incoming_buffer_;
};

} // namespace gasoline