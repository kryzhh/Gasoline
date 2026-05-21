cmake_minimum_required(VERSION 3.10)
project(gasoline)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(
    include
    core
)

add_executable(gasoline
    daemon/main.cpp

    # Networking
    core/networking/server.cpp
    core/networking/client_handler.cpp
    core/networking/send_packet.cpp
    core/networking/connect.cpp

    # Protocol
    core/protocol/packet.cpp
    core/protocol/router/packet_router.cpp
    core/protocol/handlers/hello_handler.cpp
    core/protocol/handlers/ping_handler.cpp
    core/protocol/handlers/message_handler.cpp

    # Device
    core/device/device_registry.cpp

    # Utils
    core/utils/logger.cpp
    core/utils/packet_monitor.cpp
    core/utils/device_id.cpp

    # Events
    core/events/event_bus.cpp

    # Discovery
    core/discovery/discovery_client.cpp
    platform/linux/discovery/discovery_service.cpp

    # Services
    services/message_service.cpp

    # Interface
    interface/api_server.cpp
)

# Link Avahi (Linux discovery)
target_link_libraries(gasoline
    avahi-client
    avahi-common
    pthread
)