cmake_minimum_required(VERSION 3.10)
project(gasoline)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Allow headers in include/
include_directories(include)

add_executable(gasoline
    daemon/main.cpp

    core/networking/server.cpp
    core/networking/client_handler.cpp
    core/networking/send_packet.cpp

    core/protocol/packet.cpp
    core/protocol/router/packet_router.cpp
    core/protocol/handlers/hello_handler.cpp
    core/protocol/handlers/ping_handler.cpp

    core/utils/logger.cpp
    core/utils/packet_monitor.cpp

    core/device/device_registry.cpp
)

# Platform-specific sources
if(UNIX)
    target_sources(gasoline PRIVATE
        platform/linux/discovery/discovery_service.cpp
    )
    target_link_libraries(gasoline
        avahi-client
        avahi-common
    )
endif()

if(WIN32)
    target_sources(gasoline PRIVATE
        platform/windows/discovery/discovery_service.cpp
    )
endif()