cmake_minimum_required(VERSION 3.10)
project(gasoline)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(gasoline
    daemon/main.cpp
    core/networking/server.cpp
    core/networking/client_handler.cpp
    core/protocol/packet.cpp
    core/protocol/router/packet_router.cpp
    core/utils/logger.cpp
    core/device/device_registry.cpp
)