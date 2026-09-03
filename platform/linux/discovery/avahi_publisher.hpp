#pragma once

#include "discovery/discovery_interface.hpp"

#include <atomic>
#include <cstdint>
#include <string>

extern "C" {
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/simple-watch.h>
}

namespace gasoline {

class AvahiPublisher : public IDiscoveryPublisher {
public:
    AvahiPublisher();
    ~AvahiPublisher() override;

    bool start(const std::string& device_id, uint16_t port) override;
    void stop() override;

private:
    void create_services();
    void reset_services();
    void cleanup();

    static void client_callback(AvahiClient* client, AvahiClientState state, void* userdata);
    static void entry_group_callback(AvahiEntryGroup* group, AvahiEntryGroupState state, void* userdata);

    std::string device_id_;
    uint16_t port_{0};
    AvahiSimplePoll* simple_poll_{nullptr};
    AvahiClient* client_{nullptr};
    AvahiEntryGroup* entry_group_{nullptr};
    std::atomic<bool> running_{false};
};

} // namespace gasoline

