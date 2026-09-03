#pragma once

#include "discovery/discovery_interface.hpp"

#include <atomic>
#include <string>

extern "C" {
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
}

namespace gasoline {

class AvahiBrowser : public IDiscoveryBrowser {
public:
    AvahiBrowser();
    ~AvahiBrowser() override;

    bool start(DiscoveryCallback callback) override;
    void stop() override;

private:
    void cleanup();

    static void client_callback(AvahiClient* client, AvahiClientState state, void* userdata);
    static void browse_callback(
        AvahiServiceBrowser* b,
        AvahiIfIndex interface,
        AvahiProtocol protocol,
        AvahiBrowserEvent event,
        const char* name,
        const char* type,
        const char* domain,
        AvahiLookupResultFlags flags,
        void* userdata
    );
    static void resolve_callback(
        AvahiServiceResolver* r,
        AvahiIfIndex interface,
        AvahiProtocol protocol,
        AvahiResolverEvent event,
        const char* name,
        const char* type,
        const char* domain,
        const char* host_name,
        const AvahiAddress* address,
        uint16_t port,
        AvahiStringList* txt,
        AvahiLookupResultFlags flags,
        void* userdata
    );

    DiscoveryCallback callback_;
    AvahiSimplePoll* simple_poll_{nullptr};
    AvahiClient* client_{nullptr};
    AvahiServiceBrowser* browser_{nullptr};
    std::atomic<bool> running_{false};
};

} // namespace gasoline

