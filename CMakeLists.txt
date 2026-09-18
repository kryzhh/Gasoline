cmake_minimum_required(VERSION 3.10)
project(gasoline)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
# Requires SQLite >= 3.37.0 for STRICT table support and modern security pragmas.
# Note: Upstream SQLite 3.51.3 (and backports 3.44.6 / 3.50.7) fixed the WAL-reset
# bug under concurrent manual checkpointing, but TrustStore only requires 3.37.0+
# for its feature baseline as it serializes writes and does not manually checkpoint.
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3>=3.37.0)

add_library(gasoline_trust core/trust/trust_store.cpp)
target_include_directories(gasoline_trust PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gasoline_trust PRIVATE PkgConfig::SQLITE3 pthread)

add_library(gasoline_auth core/auth/session_authentication.cpp)
target_include_directories(gasoline_auth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gasoline_auth PUBLIC gasoline_trust)

add_library(gasoline_protocol_v2 core/protocol/v2_framing.cpp)
target_include_directories(gasoline_protocol_v2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Include directories
include_directories(
    .
    include
    core
    platform/linux
)

add_executable(gasoline
    daemon/main.cpp

    # Networking
    core/networking/connection_manager.cpp
    core/networking/connection.cpp
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

    # Identity
    core/identity/device_identity.cpp

    # Events
    core/events/event_bus.cpp

    # Discovery
    core/discovery/discovery_client.cpp
    core/discovery/discovery_service.cpp
    platform/linux/discovery/avahi_publisher.cpp
    platform/linux/discovery/avahi_browser.cpp
    platform/linux/discovery/discovery_factory.cpp

    # Services
    services/message_service.cpp

    # Interface
    interface/api_server.cpp
)

# Link Avahi (Linux discovery)
target_link_libraries(gasoline
    gasoline_auth
    gasoline_trust
    gasoline_protocol_v2
    PkgConfig::SODIUM
    avahi-client
    avahi-common
    pthread
)

include(CTest)
if(BUILD_TESTING)
    add_executable(trust_store_tests tests/trust_store_tests.cpp)
    target_link_libraries(trust_store_tests gasoline_trust PkgConfig::SQLITE3 pthread)
    add_test(NAME trust_store_tests COMMAND trust_store_tests)
    set_tests_properties(trust_store_tests PROPERTIES TIMEOUT 60)

    add_executable(identity_tests
        tests/identity_tests.cpp
        core/identity/device_identity.cpp
    )
    target_link_libraries(identity_tests PkgConfig::SODIUM pthread)
    target_compile_definitions(identity_tests PRIVATE GASOLINE_IDENTITY_TESTING=1)
    # Observe/inject fsync calls in identity tests without production test hooks.
    set_property(TARGET identity_tests APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--wrap=fsync")
    add_test(NAME identity_tests COMMAND identity_tests)
    set_tests_properties(identity_tests PROPERTIES TIMEOUT 30)

    add_executable(session_tests
        tests/session_tests.cpp
        core/auth/session_authentication.cpp
        core/networking/connection.cpp
        core/networking/connection_manager.cpp
        core/networking/send_packet.cpp
        core/protocol/packet.cpp
        core/protocol/router/packet_router.cpp
        core/protocol/handlers/hello_handler.cpp
        core/protocol/handlers/ping_handler.cpp
        core/protocol/handlers/message_handler.cpp
        core/device/device_registry.cpp
        core/events/event_bus.cpp
        core/utils/logger.cpp
        core/utils/packet_monitor.cpp
        services/message_service.cpp
    )
    target_link_libraries(session_tests gasoline_protocol_v2 pthread)
    target_compile_definitions(session_tests PRIVATE GASOLINE_SESSION_TESTING=1)
    target_link_libraries(session_tests gasoline_trust PkgConfig::SQLITE3)
    add_test(NAME session_tests COMMAND session_tests)
    set_tests_properties(session_tests PROPERTIES TIMEOUT 60)

    add_executable(v2_framing_tests tests/v2_framing_tests.cpp)
    target_link_libraries(v2_framing_tests gasoline_protocol_v2)
    add_test(NAME v2_framing_tests COMMAND v2_framing_tests)
    set_tests_properties(v2_framing_tests PROPERTIES TIMEOUT 30)
endif()
