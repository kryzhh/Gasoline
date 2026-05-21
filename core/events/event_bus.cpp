#include "event_bus.hpp"

#include "../utils/logger.hpp"

#include <mutex>
#include <vector>

namespace gasoline {

namespace {
std::mutex g_events_mutex;
std::vector<MessageEvent> g_events;
constexpr size_t MAX_STORED_EVENTS = 256;
}

void EventBus::emit_message(const std::string& device_id, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lock(g_events_mutex);
        g_events.push_back({device_id, msg});
        if (g_events.size() > MAX_STORED_EVENTS) {
            g_events.erase(g_events.begin(), g_events.begin() + (g_events.size() - MAX_STORED_EVENTS));
        }
    }

    log("[EVENT] " + device_id + ": " + msg);
}

std::vector<MessageEvent> EventBus::consume_events() {
    std::lock_guard<std::mutex> lock(g_events_mutex);
    auto out = g_events;
    g_events.clear();
    return out;
}

}
