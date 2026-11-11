#include "hello_handler.hpp"

#include "../../networking/connection.hpp"
#include "../../utils/logger.hpp"

namespace gasoline {

HelloHandler::Result HelloHandler::handle(const Packet& pkt, const std::shared_ptr<Connection>& connection) {

    Result result;

    if (pkt.type != "hello" || !pkt.payload.is_object()) {
        log("Invalid hello type or payload");
        result.action = Action::DisconnectCurrent;
        return result;
    }
    const auto device_name = pkt.payload.find("device_name");
    const auto device_type = pkt.payload.find("device_type");
    if (device_name == pkt.payload.end() || !device_name->is_string() ||
        device_type == pkt.payload.end() || !device_type->is_string()) {
        log("Hello device_name and device_type must be present and strings");
        result.action = Action::DisconnectCurrent;
        return result;
    }

    if (pkt.device_id.empty()) {
        log("Missing device_id in hello packet");
        result.action = Action::DisconnectCurrent;
        return result;
    }
    // Compatibility-only setup metadata. It is deliberately not registered,
    // deduplicated, or treated as an authenticated identity.
    log("Received unverified hello metadata for session " +
        std::to_string(connection->session_id()));
    return result;
}

}
