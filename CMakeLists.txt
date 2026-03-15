cmake_minimum_required(VERSION 3.10)

project(gasoline)

set(CMAKE_CXX_STANDARD 17)

include_directories(include)

add_executable(gasoline
    daemon/main.cpp
    core/networking/server.cpp
    core/protocol/packet.cpp
    core/utils/logger.cpp
)