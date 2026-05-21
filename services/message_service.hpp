#pragma once

#include <string>

namespace gasoline {

class MessageService {
public:
    static void send_to_device(const std::string& device_id, const std::string& text);
    static void broadcast_message(const std::string& text);
};

}
