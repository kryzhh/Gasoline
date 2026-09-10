#include "core/networking/connection.hpp"
#include "core/networking/connection_manager.hpp"
#include "core/networking/send_packet.hpp"
#include "core/device/device_registry.hpp"
#include "core/events/event_bus.hpp"
#include "core/protocol/handlers/hello_handler.hpp"
#include "services/message_service.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

// The test executable never loads or changes the user's persisted identity,
// starts discovery, or connects to a network endpoint.
namespace gasoline {
std::string get_my_device_id() { return "80000000-0000-4000-8000-000000000000"; }
}

namespace {
using namespace gasoline;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;
const std::string PEER_ID = "f0000000-0000-4000-8000-000000000000";
constexpr size_t FRAME_LIMIT = 65536;

static_assert(!std::is_invocable_v<decltype(&gasoline::send_packet), int, const json&>,
              "Sending by file descriptor must not be possible");

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Predicate>
void wait_until(Predicate predicate, const std::string& message, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(2ms);
    }
}

json packet(const std::string& type, const std::string& id = PEER_ID) {
    return {{"type", type}, {"device_id", id}};
}

json hello(const std::string& id = PEER_ID) {
    auto value = packet("hello", id);
    value["payload"] = {{"device_name", "Local test peer"}, {"device_type", "test"}};
    return value;
}

struct Peer {
    int fd = -1;
    std::shared_ptr<Connection> connection;
    std::string input;

    explicit Peer(Connection::Role role = Connection::Role::Incoming, int reuse_fd = -1, bool start = true) {
        int sockets[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
        if (reuse_fd >= 0 && sockets[0] != reuse_fd) {
            require(sockets[1] != reuse_fd, "reuse target is the test peer socket");
            require(::dup2(sockets[0], reuse_fd) == reuse_fd, "dup2 failed");
            ::close(sockets[0]);
            sockets[0] = reuse_fd;
        }
        fd = sockets[1];
        const int buffer_size = 4096;
        require(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) == 0,
                "setsockopt failed");
        connection = Connection::create(sockets[0], role);
        if (start) {
            connection->start();
            require(read_packet()["type"] == "hello", "missing symmetric local hello");
        }
    }

    ~Peer() {
        if (connection) {
            const auto id = connection->session_id();
            connection->request_disconnect("test cleanup");
            connection.reset();
            const auto deadline = Clock::now() + 2s;
            while (ConnectionManager::instance().find(id) && Clock::now() < deadline) {
                std::this_thread::sleep_for(2ms);
            }
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }

    bool write(const std::string& data) {
        size_t offset = 0;
        const auto deadline = Clock::now() + 2s;
        while (offset < data.size()) {
            const auto sent = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent > 0) {
                offset += static_cast<size_t>(sent);
            } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                require(Clock::now() < deadline, "test peer write timed out");
                std::this_thread::sleep_for(1ms);
            } else {
                return false;
            }
        }
        return true;
    }

    void send(const json& value) { require(write(value.dump() + "\n"), "test peer send failed"); }

    json read_packet() {
        const auto deadline = Clock::now() + 2s;
        while (input.find('\n') == std::string::npos) {
            require(Clock::now() < deadline, "test peer receive timed out");
            pollfd descriptor{fd, POLLIN, 0};
            if (::poll(&descriptor, 1, 20) <= 0) {
                continue;
            }
            char data[4096];
            const auto count = ::recv(fd, data, sizeof(data), 0);
            require(count > 0, "unexpected disconnect");
            input.append(data, static_cast<size_t>(count));
        }
        const auto newline = input.find('\n');
        const auto result = json::parse(input.substr(0, newline));
        input.erase(0, newline + 1);
        return result;
    }

    void ready(bool use_pong = false, const std::string& id = PEER_ID) {
        send(hello(id));
        require(read_packet()["type"] == "ping", "missing handshake ping");
        send(packet(use_pong ? "pong" : "ping", id));
        if (!use_pong) {
            require(read_packet()["type"] == "pong", "missing handshake pong");
        }
        wait_until([&] {
            for (const auto& device : device_registry.get_devices_copy()) {
                if (device.session_id == connection->session_id() && device.state == DeviceState::READY) {
                    return true;
                }
            }
            return false;
        }, "session did not reach READY");
    }

    void closed() {
        wait_until([&] { return !ConnectionManager::instance().find(connection->session_id()); }, "session not cleaned up");
        wait_until([&] { return ::fcntl(connection->socket_fd(), F_GETFD) == -1 && errno == EBADF; }, "socket not closed");
        require(connection->send_packet(packet("ping")) == -1, "send after shutdown succeeded");
        for (const auto& device : device_registry.get_devices_copy()) {
            require(device.session_id != connection->session_id(), "stale device registry entry");
        }
    }
};

void normal_handshake_and_messages() {
    for (bool use_pong : {false, true}) {
        Peer peer;
        peer.ready(use_pong);
        EventBus::consume_events();
        auto message = packet("message");
        message["payload"]["text"] = "from local peer";
        peer.send(message);
        std::vector<MessageEvent> events;
        wait_until([&] { events = EventBus::consume_events(); return !events.empty(); }, "message was not delivered");
        require(events.size() == 1 && events[0].device_id == PEER_ID && events[0].text == "from local peer", "wrong message event");
        MessageService::send_to_device(PEER_ID, "to local peer");
        require(peer.read_packet()["payload"]["text"] == "to local peer", "MessageService send failed");
        MessageService::broadcast_message("local broadcast");
        require(peer.read_packet()["payload"]["text"] == "local broadcast", "broadcast failed");
    }
}

void repeated_hello_and_unexpected_packets() {
    for (bool ready_first : {false, true}) {
        for (bool change_identity : {false, true}) {
            Peer peer;
            if (ready_first) {
                peer.ready();
            } else {
                peer.send(hello());
                require(peer.read_packet()["type"] == "ping", "missing ping");
            }
            peer.send(hello(change_identity ? "a0000000-0000-4000-8000-000000000000" : PEER_ID));
            peer.closed();
            require(device_registry.get_devices_copy().empty(), "repeated hello changed peer identity");
        }
    }
    for (const auto& type : {"ping", "pong", "message"}) {
        Peer peer;
        peer.send(packet(type));
        peer.closed();
    }
    Peer changed_id;
    changed_id.ready();
    changed_id.send(packet("ping", "another-identity"));
    changed_id.closed();
}

void messages_do_not_complete_handshake() {
    Peer peer;
    peer.send(hello());
    require(peer.read_packet()["type"] == "ping", "missing ping");
    EventBus::consume_events();
    auto message = packet("message");
    message["payload"]["text"] = "message after hello";
    peer.send(message);
    wait_until([] { return !EventBus::consume_events().empty(); }, "existing post-hello message behavior changed");
    require(device_registry.get_devices_copy().at(0).state == DeviceState::HANDSHAKE_DONE,
            "message incorrectly completed handshake");
    peer.send(packet("ping"));
    require(peer.read_packet()["type"] == "pong", "ping/pong failed after message");
}

void malformed_hello_rejection() {
    std::vector<json> invalid{nullptr, json::array(), "not an object"};
    for (const auto& field : {"type", "device_id", "payload"}) {
        auto value = hello();
        value.erase(field);
        invalid.push_back(value);
    }
    const std::vector<json> non_strings{nullptr, 42, true, json::array(), json::object()};
    for (const auto& field : {"type", "device_id"}) {
        for (const auto& bad : non_strings) {
            auto value = hello();
            value.at(field) = bad;
            invalid.push_back(value);
        }
    }
    const std::vector<json> bad_payloads{
        nullptr, 42, true, "not an object", json::array(), json::object(),
        {{"device_name", "missing type"}}, {{"device_type", "missing name"}}
    };
    for (const auto& bad : bad_payloads) {
        auto value = hello();
        value.at("payload") = bad;
        invalid.push_back(value);
    }
    for (const auto& field : {"device_name", "device_type"}) {
        for (const auto& bad : non_strings) {
            auto value = hello();
            value.at("payload").at(field) = bad;
            invalid.push_back(value);
        }
    }

    Peer owner;
    owner.ready();
    for (const auto& value : invalid) {
        Peer candidate(Connection::Role::Outgoing);
        candidate.send(value);
        candidate.closed();
        const auto devices = device_registry.get_devices_copy();
        require(devices.size() == 1 && devices[0].session_id == owner.connection->session_id(),
                "invalid hello modified the existing registration");
        owner.send(packet("ping"));
        require(owner.read_packet().at("type") == "pong", "invalid hello affected another session");
    }
    Peer wrong_type;
    const Packet value{"ping", PEER_ID, hello().at("payload")};
    require(HelloHandler::handle(value, wrong_type.connection).action == HelloHandler::Action::DisconnectCurrent,
            "hello handler accepted the wrong packet type");
}

std::string padded_ping(size_t length) {
    auto value = packet("ping");
    value["padding"] = "";
    value["padding"] = std::string(length - value.dump().size(), 'x');
    const auto frame = value.dump();
    require(frame.size() == length, "incorrect test frame size");
    return frame;
}

void frame_boundaries() {
    {
        Peer peer;
        const auto greeting = hello().dump();
        require(peer.write(greeting.substr(0, 7)), "fragment send failed");
        require(peer.write(greeting.substr(7) + "\n" + packet("ping").dump() + "\n"), "coalesced send failed");
        require(peer.read_packet()["type"] == "ping", "fragmented hello failed");
        require(peer.read_packet()["type"] == "pong", "coalesced ping failed");
        const auto boundary = padded_ping(FRAME_LIMIT);
        require(peer.write(boundary), "boundary send failed");
        require(peer.write("\n" + boundary + "\n"), "boundary delimiter send failed");
        require(peer.read_packet()["type"] == "pong", "exact-size frame rejected");
        require(peer.read_packet()["type"] == "pong", "coalesced exact-size frame rejected");
    }
    for (bool terminated : {false, true}) {
        Peer peer;
        peer.ready();
        peer.write(padded_ping(FRAME_LIMIT + 1) + (terminated ? "\n" : ""));
        peer.closed();
    }
    Peer invalid;
    invalid.write("{broken-json}\n");
    invalid.closed();
}

void bounded_outgoing_serialization() {
    Peer peer;
    peer.ready();
    auto rejected = [&](const json& value) {
        require(peer.connection->send_packet(value) == -1, "oversized outgoing packet was accepted");
        require(!peer.connection->is_stopping(), "local oversize rejection stopped the session");
        char byte;
        require(::recv(peer.fd, &byte, 1, MSG_DONTWAIT) == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK), "rejected packet wrote a partial frame");
    };

    auto huge = packet("message");
    huge["payload"] = std::string(4 * 1024 * 1024, 'x');
    rejected(huge);
    // A full dump would throw on this later field. Successful rejection proves
    // serialization stops at the cap before examining the rest of the packet.
    huge["z_tail"] = std::string(1, static_cast<char>(0xff));
    rejected(huge);
    auto escaped = packet("message");
    escaped["payload"] = std::string(11000, '\0');
    rejected(escaped);
    auto many_elements = packet("message");
    many_elements["payload"] = std::vector<int>(40000, 0);
    rejected(many_elements);
    rejected(json::parse(padded_ping(FRAME_LIMIT + 1)));

    const auto boundary = json::parse(padded_ping(FRAME_LIMIT));
    auto sending = std::async(std::launch::async, [&] { return peer.connection->send_packet(boundary); });
    require(peer.read_packet() == boundary, "outgoing boundary frame was corrupted");
    require(sending.get() == static_cast<ssize_t>(FRAME_LIMIT + 1), "exact outgoing limit was rejected");
    MessageService::send_to_device(PEER_ID, "after oversize rejection");
    require(peer.read_packet().at("payload").at("text") == "after oversize rejection",
            "session failed after local oversize rejection");
}

void duplicate_ownership() {
    Peer old(Connection::Role::Incoming);
    old.ready();
    Peer preferred(Connection::Role::Outgoing);
    preferred.ready();
    old.closed();
    const auto current_id = preferred.connection->session_id();
    device_registry.remove_device(old.connection->session_id());
    require(!device_registry.set_state_for_session(old.connection->session_id(), DeviceState::DISCONNECTED), "stale state update succeeded");
    require(device_registry.get_devices_copy().at(0).session_id == current_id, "stale cleanup removed new owner");
    for (auto role : {Connection::Role::Incoming, Connection::Role::Outgoing}) {
        Peer duplicate(role);
        duplicate.send(hello());
        duplicate.closed();
        require(device_registry.get_devices_copy().at(0).session_id == current_id, "duplicate replaced incumbent");
    }
    preferred.send(packet("ping"));
    require(preferred.read_packet()["type"] == "pong", "owner stopped responding");
}

void descriptor_reuse_and_lifetime() {
    Peer old;
    old.ready();
    const auto stale_device = device_registry.get_devices_copy().at(0);
    const auto old_id = old.connection->session_id();
    const int old_fd = old.connection->socket_fd();
    old.connection->request_disconnect("descriptor reuse test");
    old.closed();
    Peer replacement(Connection::Role::Incoming, old_fd);
    replacement.ready();
    require(replacement.connection->session_id() != old_id, "session ID reused");
    require(send_packet(stale_device.connection.lock(), packet("ping")) == -1, "stale snapshot sent on reused fd");
    old.connection->request_disconnect("late shutdown");
    device_registry.remove_device(old_id);
    ConnectionManager::instance().unregister_connection(old_id);
    old.connection.reset();
    require(ConnectionManager::instance().find(replacement.connection->session_id()) == replacement.connection, "late unregister removed replacement");
    replacement.send(packet("ping"));
    require(replacement.read_packet()["type"] == "pong", "stale destructor or shutdown touched reused fd");
    require(send_packet(nullptr, packet("ping")) == -1, "missing session send succeeded");
}

void simultaneous_duplicate_registration() {
    for (int iteration = 0; iteration < 10; ++iteration) {
        Peer incoming(Connection::Role::Incoming);
        Peer outgoing(Connection::Role::Outgoing);
        std::atomic<bool> go{false};
        auto first = std::async(std::launch::async, [&] {
            while (!go.load()) { std::this_thread::yield(); }
            incoming.send(hello());
        });
        auto second = std::async(std::launch::async, [&] {
            while (!go.load()) { std::this_thread::yield(); }
            outgoing.send(hello());
        });
        go.store(true);
        first.get();
        second.get();
        require(outgoing.read_packet()["type"] == "ping", "preferred connection did not win");
        outgoing.send(packet("ping"));
        require(outgoing.read_packet()["type"] == "pong", "preferred connection handshake failed");
        incoming.closed();
        const auto devices = device_registry.get_devices_copy();
        require(devices.size() == 1 && devices[0].session_id == outgoing.connection->session_id(), "concurrent registration lost ownership");
    }
}

void worker_owns_connection() {
    Peer peer;
    peer.ready();
    const auto id = peer.connection->session_id();
    std::weak_ptr<Connection> weak = peer.connection;
    peer.connection.reset();
    require(!weak.expired(), "worker lost its Connection");
    peer.send(packet("ping"));
    require(peer.read_packet()["type"] == "pong", "worker failed after external owner released");
    ::shutdown(peer.fd, SHUT_RDWR);
    wait_until([&] { return weak.expired(); }, "worker retained Connection after disconnect");
    require(!ConnectionManager::instance().find(id), "manager retained dead session");
    require(device_registry.get_devices_copy().empty(), "registry retained dead worker");
    Peer unstarted(Connection::Role::Incoming, -1, false);
    const auto unstarted_id = unstarted.connection->session_id();
    const auto unstarted_fd = unstarted.connection->socket_fd();
    unstarted.connection.reset();
    require(!ConnectionManager::instance().find(unstarted_id), "unstarted session leaked");
    require(::fcntl(unstarted_fd, F_GETFD) == -1 && errno == EBADF, "unstarted socket leaked");
}

void concurrent_writes_and_disconnect() {
    {
        Peer peer;
        peer.ready();
        std::vector<std::future<void>> writers;
        for (int writer = 0; writer < 4; ++writer) {
            writers.push_back(std::async(std::launch::async, [connection = peer.connection, writer] {
                for (int i = 0; i < 10; ++i) {
                    require(connection->send_packet({{"type", "message"}, {"writer", writer}, {"index", i}}) > 0, "serialized send failed");
                }
            }));
        }
        bool seen[4][10]{};
        for (int i = 0; i < 40; ++i) {
            const auto value = peer.read_packet();
            const int writer = value.at("writer");
            const int index = value.at("index");
            require(writer >= 0 && writer < 4 && index >= 0 && index < 10 && !seen[writer][index], "corrupt or duplicated concurrent frame");
            seen[writer][index] = true;
        }
        for (auto& writer : writers) { writer.get(); }
    }
    for (bool peer_disconnect : {false, true}) {
        Peer peer;
        peer.ready();
        std::atomic<bool> entering_send{false};
        auto blocked = std::async(std::launch::async, [&] {
            entering_send.store(true);
            return peer.connection->send_packet({{"type", "message"}, {"padding", std::string(60000, 'x')}});
        });
        wait_until([&] { return entering_send.load(); }, "writer did not start");
        require(blocked.wait_for(100ms) == std::future_status::timeout, "test did not produce a blocked writer");
        std::vector<std::future<ssize_t>> queued;
        for (int i = 0; i < 4; ++i) {
            queued.push_back(std::async(std::launch::async, [&] { return peer.connection->send_packet(packet("ping")); }));
        }
        if (peer_disconnect) {
            ::shutdown(peer.fd, SHUT_RDWR);
        } else {
            peer.connection->request_disconnect("interrupt blocked writer");
        }
        require(blocked.wait_for(2s) == std::future_status::ready && blocked.get() == -1, "blocked writer did not fail promptly");
        for (auto& result : queued) {
            require(result.wait_for(2s) == std::future_status::ready && result.get() == -1, "queued send succeeded after shutdown");
        }
        peer.closed();
    }
}

void connect_disconnect_cycles() {
    for (int i = 0; i < 50; ++i) {
        Peer peer;
        peer.ready(i % 2 == 0);
        ::shutdown(peer.fd, SHUT_RDWR);
        peer.closed();
        require(device_registry.get_devices_copy().empty(), "registry grew across reconnects");
    }
}

void handshake_deadlines() {
    auto verify_expiry = [](Peer& peer, Clock::time_point begin, bool trickle, const std::string& label) {
        auto next_byte = Clock::now();
        while (!peer.connection->is_stopping()) {
            const auto now = Clock::now();
            require(now - begin < 13s, label + " handshake did not expire");
            if (trickle && now >= next_byte) {
                peer.write(" ");
                next_byte = now + 100ms;
            }
            std::this_thread::sleep_for(2ms);
        }
        const auto elapsed = Clock::now() - begin;
        require(elapsed >= 9s && elapsed < 13s, label + " handshake expired outside its own deadline window");
        peer.closed();
    };
    const auto silent_begin = Clock::now();
    Peer silent;
    auto silent_expiry = std::async(std::launch::async, [&] { verify_expiry(silent, silent_begin, false, "silent"); });
    const auto hello_begin = Clock::now();
    Peer hello_only;
    hello_only.send(hello());
    require(hello_only.read_packet()["type"] == "ping", "missing ping");
    auto hello_expiry = std::async(std::launch::async, [&] { verify_expiry(hello_only, hello_begin, false, "hello-only"); });
    const auto trickle_begin = Clock::now();
    Peer trickle;
    auto trickle_expiry = std::async(std::launch::async, [&] { verify_expiry(trickle, trickle_begin, true, "trickling"); });
    const auto idle_begin = Clock::now();
    Peer idle_ready;
    const std::string idle_id = "e0000000-0000-4000-8000-000000000000";
    idle_ready.ready(false, idle_id);
    Peer stalled_writer;
    stalled_writer.ready(false, "d0000000-0000-4000-8000-000000000000");
    auto blocked = std::async(std::launch::async, [&] {
        const auto begin = Clock::now();
        const auto result = stalled_writer.connection->send_packet({{"type", "message"}, {"padding", std::string(60000, 'x')}});
        const auto elapsed = Clock::now() - begin;
        require(result == -1 && elapsed >= 9s && elapsed < 13s, "write expired outside its own deadline window");
    });
    silent_expiry.get();
    hello_expiry.get();
    trickle_expiry.get();
    require(blocked.wait_for(2s) == std::future_status::ready, "write timeout did not fire");
    blocked.get();
    stalled_writer.closed();
    std::this_thread::sleep_until(idle_begin + 11s);
    idle_ready.send(packet("ping", idle_id));
    require(idle_ready.read_packet()["type"] == "pong", "handshake deadline closed a READY session");
}

} // namespace

int main() {
    try {
        const std::pair<const char*, void(*)()> tests[] = {
            {"hello, ping/pong, READY and bidirectional messages", normal_handshake_and_messages},
            {"repeated hello and unexpected identity/packet rejection", repeated_hello_and_unexpected_packets},
            {"post-hello messages preserve the incomplete handshake state", messages_do_not_complete_handshake},
            {"malformed hello validation and controlled rejection", malformed_hello_rejection},
            {"fragmented, coalesced, boundary and oversized frames", frame_boundaries},
            {"bounded outgoing serialization and atomic frame rejection", bounded_outgoing_serialization},
            {"UUID duplicate ownership and stale registry operations", duplicate_ownership},
            {"simultaneous opposite-direction registration", simultaneous_duplicate_registration},
            {"descriptor reuse, stale handles and no raw fallback", descriptor_reuse_and_lifetime},
            {"worker strong ownership and unstarted cleanup", worker_owns_connection},
            {"concurrent writes and shutdown around blocked sends", concurrent_writes_and_disconnect},
            {"50 connect/disconnect cycles", connect_disconnect_cycles},
            {"10-second handshake/write deadlines and idle READY survival", handshake_deadlines},
        };
        for (const auto& test : tests) {
            test.second();
            require(device_registry.get_devices_copy().empty(), "registry not empty after test");
            std::cout << "PASS: " << test.first << std::endl;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }
}
