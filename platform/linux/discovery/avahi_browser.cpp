#include "avahi_browser.hpp"
#include "utils/logger.hpp"

extern "C" {
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
}

namespace gasoline {

namespace {

struct ResolverGuard {
    AvahiServiceResolver* resolver;
    ~ResolverGuard() {
        if (resolver) {
            avahi_service_resolver_free(resolver);
        }
    }
};

} // namespace

AvahiBrowser::AvahiBrowser() = default;

AvahiBrowser::~AvahiBrowser() {
    stop();
}

void AvahiBrowser::stop() {
    if (running_.exchange(false)) {
        if (simple_poll_) {
            avahi_simple_poll_quit(simple_poll_);
        }
    }
}

void AvahiBrowser::cleanup() {
    if (browser_) {
        avahi_service_browser_free(browser_);
        browser_ = nullptr;
    }
    if (client_) {
        avahi_client_free(client_);
        client_ = nullptr;
    }
    if (simple_poll_) {
        avahi_simple_poll_free(simple_poll_);
        simple_poll_ = nullptr;
    }
}

void AvahiBrowser::resolve_callback(
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
) {
    // Guarantees resolver is unconditionally freed on ANY return path
    ResolverGuard guard{r};

    auto* self = static_cast<AvahiBrowser*>(userdata);
    const std::string service_name = name ? name : "unknown";

    if (event != AVAHI_RESOLVER_FOUND) {
        log("AvahiBrowser: Failed to resolve service '" + service_name + "': " +
            (self->client_ ? avahi_strerror(avahi_client_errno(self->client_)) : "unknown error"));
        return;
    }

    if (!address || address->proto != AVAHI_PROTO_INET) {
        return;
    }

    char addr_str[AVAHI_ADDRESS_STR_MAX];
    avahi_address_snprint(addr_str, sizeof(addr_str), address);
    std::string ip = addr_str;

    if (ip == "127.0.0.1") {
        return;
    }

    // Extract device_id from TXT record (authoritative)
    std::string peer_device_id;
    if (txt) {
        AvahiStringList* entry = avahi_string_list_find(txt, "device_id");
        if (entry) {
            char* key = nullptr;
            char* value = nullptr;
            size_t size = 0;
            if (avahi_string_list_get_pair(entry, &key, &value, &size) == 0) {
                if (value) {
                    peer_device_id.assign(value, size);
                    avahi_free(value);
                }
                if (key) {
                    avahi_free(key);
                }
            }
        }
    }

    if (peer_device_id.empty()) {
        log("AvahiBrowser: Discovered service '" + service_name + "' has no device_id in TXT record; ignoring");
        return;
    }

    if (self->callback_) {
        DiscoveredDevice dev;
        dev.device_id = std::move(peer_device_id);
        dev.device_name = service_name;
        dev.ip_address = std::move(ip);
        dev.port = port;
        self->callback_(dev);
    }
}

void AvahiBrowser::browse_callback(
    AvahiServiceBrowser* b,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiBrowserEvent event,
    const char* name,
    const char* type,
    const char* domain,
    AvahiLookupResultFlags flags,
    void* userdata
) {
    auto* self = static_cast<AvahiBrowser*>(userdata);

    switch (event) {
        case AVAHI_BROWSER_NEW: {
            log(std::string("AvahiBrowser: Discovered service: ") + (name ? name : "unknown"));

            AvahiServiceResolver* resolver = avahi_service_resolver_new(
                self->client_,
                interface,
                protocol,
                name,
                type,
                domain,
                AVAHI_PROTO_INET,
                (AvahiLookupFlags)0,
                resolve_callback,
                self
            );

            if (!resolver) {
                log(std::string("AvahiBrowser: Failed to create service resolver: ") +
                    avahi_strerror(avahi_client_errno(self->client_)));
            }
            break;
        }

        case AVAHI_BROWSER_REMOVE:
            log(std::string("AvahiBrowser: Service removed: ") + (name ? name : "unknown"));
            break;

        case AVAHI_BROWSER_FAILURE:
            log(std::string("AvahiBrowser: Browser failure: ") +
                avahi_strerror(avahi_client_errno(self->client_)));
            if (self->simple_poll_) {
                avahi_simple_poll_quit(self->simple_poll_);
            }
            break;

        case AVAHI_BROWSER_ALL_FOR_NOW:
        case AVAHI_BROWSER_CACHE_EXHAUSTED:
            break;
    }
}

void AvahiBrowser::client_callback(
    AvahiClient* client,
    AvahiClientState state,
    void* userdata
) {
    auto* self = static_cast<AvahiBrowser*>(userdata);
    self->client_ = client;

    switch (state) {
        case AVAHI_CLIENT_S_RUNNING: {
            log("AvahiBrowser: Client running, starting service browser for _gasoline._tcp...");
            if (self->browser_) {
                avahi_service_browser_free(self->browser_);
                self->browser_ = nullptr;
            }

            self->browser_ = avahi_service_browser_new(
                client,
                AVAHI_IF_UNSPEC,
                AVAHI_PROTO_INET,
                "_gasoline._tcp",
                nullptr,
                (AvahiLookupFlags)0,
                browse_callback,
                self
            );

            if (!self->browser_) {
                log(std::string("AvahiBrowser: Failed to create service browser: ") +
                    avahi_strerror(avahi_client_errno(client)));
            }
            break;
        }

        case AVAHI_CLIENT_FAILURE:
            log(std::string("AvahiBrowser: Client failure: ") +
                avahi_strerror(avahi_client_errno(client)));
            if (self->simple_poll_) {
                avahi_simple_poll_quit(self->simple_poll_);
            }
            break;

        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
        case AVAHI_CLIENT_CONNECTING:
            break;
    }
}

bool AvahiBrowser::start(DiscoveryCallback callback) {
    callback_ = std::move(callback);

    cleanup();

    simple_poll_ = avahi_simple_poll_new();
    if (!simple_poll_) {
        log("AvahiBrowser: Failed to create Avahi simple poll");
        return false;
    }

    int error = 0;
    client_ = avahi_client_new(
        avahi_simple_poll_get(simple_poll_),
        (AvahiClientFlags)0,
        client_callback,
        this,
        &error
    );

    if (!client_) {
        log(std::string("AvahiBrowser: Failed to create Avahi client: ") + avahi_strerror(error));
        cleanup();
        return false;
    }

    running_.store(true);
    log("AvahiBrowser: Starting event loop...");
    avahi_simple_poll_loop(simple_poll_);

    cleanup();
    return true;
}

} // namespace gasoline
