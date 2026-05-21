#include "api_server.hpp"

#include "../core/device/device_registry.hpp"
#include "../core/events/event_bus.hpp"
#include "../core/utils/device_id.hpp"
#include "../core/utils/logger.hpp"
#include "../services/message_service.hpp"

#include <arpa/inet.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

namespace gasoline {

namespace {

constexpr int API_PORT = 42667;
constexpr int API_BUFFER_SIZE = 4096;

std::string device_state_to_string(DeviceState state) {
    switch (state) {
    case DeviceState::CONNECTING:
        return "CONNECTING";
    case DeviceState::HANDSHAKE_DONE:
        return "HANDSHAKE_DONE";
    case DeviceState::READY:
        return "READY";
    case DeviceState::DISCONNECTED:
        return "DISCONNECTED";
    default:
        return "UNKNOWN";
    }
}

bool send_http_response(int client_fd, int status_code, const std::string& status_text,
                        const std::string& body, const std::string& content_type = "application/json") {
    const std::string response =
        "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n" +
        "Access-Control-Allow-Origin: *\r\n" +
        "Access-Control-Allow-Headers: Content-Type\r\n" +
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n" +
        "Content-Type: " + content_type + "\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Connection: close\r\n\r\n" +
        body;

    ssize_t total_sent = 0;
    while (total_sent < static_cast<ssize_t>(response.size())) {
        const ssize_t sent = send(client_fd, response.c_str() + total_sent, response.size() - total_sent, 0);
        if (sent <= 0)
            return false;
        total_sent += sent;
    }
    return true;
}

int extract_content_length(const std::string& headers) {
    const std::string needle = "Content-Length:";
    const size_t pos = headers.find(needle);
    if (pos == std::string::npos)
        return 0;

    size_t value_start = pos + needle.size();
    while (value_start < headers.size() && headers[value_start] == ' ')
        value_start++;

    size_t value_end = headers.find("\r\n", value_start);
    if (value_end == std::string::npos)
        return 0;

    try {
        return std::stoi(headers.substr(value_start, value_end - value_start));
    } catch (...) {
        return 0;
    }
}

bool recv_http_request(int client_fd, std::string& out_request) {
    out_request.clear();
    char buffer[API_BUFFER_SIZE];

    while (out_request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0)
            return false;
        out_request.append(buffer, bytes);
    }

    const size_t headers_end = out_request.find("\r\n\r\n");
    if (headers_end == std::string::npos) {
        return false;
    }
    const std::string headers = out_request.substr(0, headers_end + 4);
    const int content_length = extract_content_length(headers);
    if (content_length < 0 || content_length > API_BUFFER_SIZE) {
        return false;
    }
    const size_t target_size = headers_end + 4 + static_cast<size_t>(content_length);

    while (out_request.size() < target_size) {
        const ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0)
            return false;
        out_request.append(buffer, bytes);
    }

    return true;
}

std::string get_request_body(const std::string& request) {
    const size_t headers_end = request.find("\r\n\r\n");
    if (headers_end == std::string::npos)
        return "";
    return request.substr(headers_end + 4);
}

void handle_get_devices(int client_fd) {
    const auto devices = device_registry.get_devices_copy();

    nlohmann::json out = nlohmann::json::array();
    for (const auto& device : devices) {
        nlohmann::json item;
        item["device_id"] = device.device_id;
        item["device_name"] = device.device_name;
        item["device_type"] = device.device_type;
        item["state"] = device_state_to_string(device.state);
        item["is_self"] = device.device_id == get_my_device_id();
        out.push_back(item);
    }

    send_http_response(client_fd, 200, "OK", out.dump());
}

void handle_get_events(int client_fd) {
    const auto events = EventBus::consume_events();

    nlohmann::json out = nlohmann::json::array();
    for (const auto& event : events) {
        nlohmann::json item;
        item["device_id"] = event.device_id;
        item["text"] = event.text;
        out.push_back(item);
    }

    send_http_response(client_fd, 200, "OK", out.dump());
}

void handle_post_send(int client_fd, const std::string& request_body) {
    try {
        const auto body = nlohmann::json::parse(request_body);

        if (!body.contains("device_id") || !body.contains("text")) {
            send_http_response(client_fd, 400, "Bad Request",
                               R"({"error":"'device_id' and 'text' are required"})");
            return;
        }

        const std::string device_id = body["device_id"].get<std::string>();
        const std::string text = body["text"].get<std::string>();

        MessageService::send_to_device(device_id, text);
        send_http_response(client_fd, 200, "OK", R"({"status":"queued"})");
    } catch (const std::exception& e) {
        send_http_response(client_fd, 400, "Bad Request",
                           std::string("{\"error\":\"") + e.what() + "\"}");
    }
}

void handle_api_client(int client_fd) {
    std::string request;
    if (!recv_http_request(client_fd, request)) {
        log("Incomplete HTTP request");
        send_http_response(client_fd, 400, "Bad Request", R"({"error":"incomplete request"})");
        close(client_fd);
        return;
    }

    if (request.find("\r\n\r\n") == std::string::npos) {
        log("Incomplete HTTP request");
        close(client_fd);
        return;
    }

    const size_t line_end = request.find("\r\n");
    if (line_end == std::string::npos) {
        send_http_response(client_fd, 400, "Bad Request", R"({"error":"invalid request line"})");
        close(client_fd);
        return;
    }

    const std::string request_line = request.substr(0, line_end);
    if (request_line.rfind("OPTIONS ", 0) == 0) {
        send_http_response(client_fd, 204, "No Content", "", "text/plain");
    } else if (request_line.rfind("GET /devices", 0) == 0) {
        handle_get_devices(client_fd);
    } else if (request_line.rfind("GET /events", 0) == 0) {
        handle_get_events(client_fd);
    } else if (request_line.rfind("POST /send", 0) == 0) {
        handle_post_send(client_fd, get_request_body(request));
    } else {
        send_http_response(client_fd, 404, "Not Found", R"({"error":"endpoint not found"})");
    }

    close(client_fd);
}

} // namespace

void bootstrap_api_server() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log("API server socket creation failed");
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(API_PORT);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        log(std::string("API bind failed: ") + std::strerror(errno));
        close(server_fd);
        return;
    }

    if (listen(server_fd, 8) < 0) {
        log(std::string("API listen failed: ") + std::strerror(errno));
        close(server_fd);
        return;
    }

    log("API server listening on port " + std::to_string(API_PORT));

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0)
            continue;

        std::thread([client_fd]() {
            handle_api_client(client_fd);
        }).detach();
    }
}

}
