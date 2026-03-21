#include "discovery_client.hpp"
#include "../utils/logger.hpp"
#include "../networking/connect.hpp"

#include <set>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>

extern "C" {
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/error.h>
}

namespace gasoline {

static AvahiSimplePoll* simple_poll = nullptr; // Avahi Poll
static std::set<std::string> discovered_ips; // IPs discovered
static std::set<std::string> connected_ips; // IPs already connected to

// Gets local IP
std::string get_local_ip() {
    struct ifaddrs *ifaddr, *ifa;

    if (getifaddrs(&ifaddr) == -1) {
        return "";
    }

    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
            std::string ip = inet_ntoa(sa->sin_addr);

            // Skip loopback
            if (ip != "127.0.0.1") {
                freeifaddrs(ifaddr);
                return ip;
            }
        }
    }

    freeifaddrs(ifaddr);
    return "";
}

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
) {

    if (event == AVAHI_RESOLVER_FOUND) { // If address resolved

        char addr[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(addr, sizeof(addr), address);

        std::string ip = addr;

        // Skip IPv6
        if (ip.find(":") != std::string::npos) {
            return;
        }

        // Skip loopback
        if (ip == "127.0.0.1") {
            return;
        }

        std::string local_ip = get_local_ip();
        std::string subnet = local_ip.substr(0, local_ip.find_last_of('.') + 1);

        // ONLY allow same subnet
        if (!(ip.rfind(subnet, 0) == 0)) {
            return;
        }

        // Avoid duplicate discovery
        if (discovered_ips.count(ip)) {
            return;
        }
        discovered_ips.insert(ip);

        // std::string local_ip = get_local_ip();
        // if (ip == local_ip) {
        //     log("Skipping self connection");
        //     return;
        // }

        // Avoid duplicate connections
        if (connected_ips.count(ip)) {
            log("Already connected, skipping");
            return;
        }
        connected_ips.insert(ip);

        // Log it
        log(std::string("Name: ") + name);
        log(std::string("IP: ") + addr);
        log(std::string("Port: ") + std::to_string(port));

        log("Attempting connection to: " + ip);
        connect_to_device(ip, port);

    } else {
        log("Failed to resolve service");
    }

    avahi_service_resolver_free(r);
}

// Browser callback
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
) {

    AvahiClient* client = (AvahiClient*)userdata;

    switch (event) {

        case AVAHI_BROWSER_NEW: // If a new service found
            log(std::string("Discovered service: ") + name);

            avahi_service_resolver_new( // Resolve it
                client,
                interface,
                protocol,
                name,
                type,
                domain,
                AVAHI_PROTO_UNSPEC,
                (AvahiLookupFlags)0,
                resolve_callback,
                nullptr
            );
            break;

        case AVAHI_BROWSER_REMOVE:
            log(std::string("Service removed: ") + name);
            break;

        case AVAHI_BROWSER_FAILURE:
            log("Browser failure");
            break;

        default:
            break;
    }
}

// Client callback
static void client_callback(
    AvahiClient* client,
    AvahiClientState state,
    void* userdata
) {

    if (state == AVAHI_CLIENT_S_RUNNING) {

        log("Avahi client running. Starting browse...");

        AvahiServiceBrowser* browser = avahi_service_browser_new(
            client,
            AVAHI_IF_UNSPEC,
            AVAHI_PROTO_UNSPEC,
            "_gasoline._tcp",
            nullptr,
            (AvahiLookupFlags)0,
            browse_callback,
            client
        );

        if (!browser) {
            log("Failed to create service browser");
        }
    }

    else if (state == AVAHI_CLIENT_FAILURE) {
        log("Avahi client failure");
        avahi_simple_poll_quit(simple_poll);
    }
}

// Start discovery client
void DiscoveryClient::start() {

    log("Starting discovery client...");

    int error;

    simple_poll = avahi_simple_poll_new();

    if (!simple_poll) {
        log("Failed to create Avahi poll object");
        return;
    }

    AvahiClient* client = avahi_client_new(
        avahi_simple_poll_get(simple_poll),
        (AvahiClientFlags)0,
        client_callback,
        nullptr,
        &error
    );

    if (!client) {
        log(std::string("Failed to create Avahi client: ") +
            avahi_strerror(error));
        return;
    }

    log("Discovery client running...");

    avahi_simple_poll_loop(simple_poll);
}

}