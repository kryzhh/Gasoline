#include "message_handler.hpp"
#include "../../events/event_bus.hpp"
#include "../../utils/logger.hpp"

namespace gasoline {

void MessageHandler::handle(const Packet& pkt, int socket_fd) {

    if (!pkt.payload.contains("text")) { // Packet should be valid and have a text field
        log("Message packet missing 'text'");
        return;
    }

    std::string msg = pkt.payload["text"];

    EventBus::emit_message(pkt.device_id, msg);
}

}