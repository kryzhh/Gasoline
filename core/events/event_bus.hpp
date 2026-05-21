#pragma once

#include <string>
#include <vector>

namespace gasoline {

struct MessageEvent {
    std::string device_id;
    std::string text;
};

class EventBus {
public:
    static void emit_message(const std::string& device_id, const std::string& msg);
    static std::vector<MessageEvent> consume_events();
};

}
