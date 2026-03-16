#include <gasoline/discovery_service.hpp>
#include "../core/utils/logger.hpp"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>

namespace gasoline {

// Avahi event loop handler
static AvahiSimplePoll* simple_poll = nullptr;

/*
    This function is called when the service registration state changes.
    Avahi uses callbacks for almost everything.
*/
static void entry_group_callback(
    AvahiEntryGroup* g,
    AvahiEntryGroupState state,
    void* userdata
) {

    if (state == AVAHI_ENTRY_GROUP_ESTABLISHED) { // Succesfully established service
        log("mDNS service established");
    }

}

// Actually registers the service.
static void create_services(AvahiClient* client) {
    AvahiEntryGroup* group = avahi_entry_group_new( // Create a group of services
        client,
        entry_group_callback,
        nullptr
    );

    if (!group)
        return;

    avahi_entry_group_add_service( // Advertises the daemon to the network
        group, // service group
        AVAHI_IF_UNSPEC, // all network interfaces
        AVAHI_PROTO_UNSPEC, // IPv4 and IPv6
        (AvahiPublishFlags)0, 
        "Gasoline", // Service name
        "_gasoline._tcp", // Service type
        nullptr, // Holds the domain (here, local)
        nullptr, // Host
        42666, // Port
        nullptr // Extra metadata
    );

    avahi_entry_group_commit(group); // Start advertising this service
}

// This callback runs when the Avahi client changes state
static void client_callback( 
    AvahiClient* client,
    AvahiClientState state,
    void* userdata
) {

    if (state == AVAHI_CLIENT_S_RUNNING) { // If avahi ready
        log("Avahi client running");
        create_services(client); // Advertise this

    }

}

void DiscoveryService::start() {
    int error;
    simple_poll = avahi_simple_poll_new(); // Event loop. Required to monitor network events
    if (!simple_poll) {
        log("Failed to create Avahi poll");
        return;
    }

    AvahiClient* client = avahi_client_new( // Connects program to the Avahi daemon running on the system
        avahi_simple_poll_get(simple_poll),
        (AvahiClientFlags)0,
        client_callback,
        nullptr,
        &error
    );

    if (!client) {
        log("Failed to create Avahi client");
        return;
    }

    log("Discovery service started");

}

}