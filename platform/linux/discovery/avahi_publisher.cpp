#include "avahi_publisher.hpp"
#include "utils/logger.hpp"

extern "C" {
#include <avahi-common/error.h>
#include <avahi-common/strlst.h>
}

namespace gasoline {

AvahiPublisher::AvahiPublisher() = default;

AvahiPublisher::~AvahiPublisher() {
    stop();
}

void AvahiPublisher::stop() {
    if (running_.exchange(false)) {
        if (simple_poll_) {
            avahi_simple_poll_quit(simple_poll_);
        }
    }
}

void AvahiPublisher::cleanup() {
    reset_services();
    if (client_) {
        avahi_client_free(client_);
        client_ = nullptr;
    }
    if (simple_poll_) {
        avahi_simple_poll_free(simple_poll_);
        simple_poll_ = nullptr;
    }
}

void AvahiPublisher::reset_services() {
    if (entry_group_) {
        avahi_entry_group_reset(entry_group_);
        avahi_entry_group_free(entry_group_);
        entry_group_ = nullptr;
    }
}

void AvahiPublisher::create_services() {
    if (!client_) {
        return;
    }

    if (!entry_group_) {
        entry_group_ = avahi_entry_group_new(client_, entry_group_callback, this);
        if (!entry_group_) {
            log(std::string("AvahiPublisher: Failed to create entry group: ") +
                avahi_strerror(avahi_client_errno(client_)));
            return;
        }
    }

    if (avahi_entry_group_is_empty(entry_group_)) {
        const std::string service_name = "Gasoline-" + device_id_;

        AvahiStringList* txt = nullptr;
        txt = avahi_string_list_add_pair(txt, "device_id", device_id_.c_str());

        const int ret = avahi_entry_group_add_service_strlst(
            entry_group_,
            AVAHI_IF_UNSPEC,
            AVAHI_PROTO_UNSPEC,
            (AvahiPublishFlags)0,
            service_name.c_str(),
            "_gasoline._tcp",
            nullptr,
            nullptr,
            port_,
            txt
        );

        avahi_string_list_free(txt);

        if (ret < 0) {
            log(std::string("AvahiPublisher: Failed to add service: ") + avahi_strerror(ret));
            reset_services();
            return;
        }

        const int commit_ret = avahi_entry_group_commit(entry_group_);
        if (commit_ret < 0) {
            log(std::string("AvahiPublisher: Failed to commit entry group: ") + avahi_strerror(commit_ret));
            reset_services();
            return;
        }

        log("AvahiPublisher: Registered service '" + service_name + "' with device_id TXT record");
    }
}

void AvahiPublisher::entry_group_callback(
    AvahiEntryGroup* group,
    AvahiEntryGroupState state,
    void* userdata
) {
    auto* self = static_cast<AvahiPublisher*>(userdata);
    switch (state) {
        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            log("AvahiPublisher: mDNS service established for device " + self->device_id_);
            break;
        case AVAHI_ENTRY_GROUP_COLLISION:
            log("AvahiPublisher: Service name collision detected");
            break;
        case AVAHI_ENTRY_GROUP_FAILURE:
            log(std::string("AvahiPublisher: Entry group failure: ") +
                avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(group))));
            if (self->simple_poll_) {
                avahi_simple_poll_quit(self->simple_poll_);
            }
            break;
        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:
            break;
    }
}

void AvahiPublisher::client_callback(
    AvahiClient* client,
    AvahiClientState state,
    void* userdata
) {
    auto* self = static_cast<AvahiPublisher*>(userdata);
    self->client_ = client;

    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            log("AvahiPublisher: Client running, creating services...");
            self->create_services();
            break;
        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
            self->reset_services();
            break;
        case AVAHI_CLIENT_FAILURE:
            log(std::string("AvahiPublisher: Client failure: ") +
                avahi_strerror(avahi_client_errno(client)));
            if (self->simple_poll_) {
                avahi_simple_poll_quit(self->simple_poll_);
            }
            break;
        case AVAHI_CLIENT_CONNECTING:
            break;
    }
}

bool AvahiPublisher::start(const std::string& device_id, uint16_t port) {
    device_id_ = device_id;
    port_ = port;

    cleanup();

    simple_poll_ = avahi_simple_poll_new();
    if (!simple_poll_) {
        log("AvahiPublisher: Failed to create Avahi simple poll");
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
        log(std::string("AvahiPublisher: Failed to create Avahi client: ") + avahi_strerror(error));
        cleanup();
        return false;
    }

    running_.store(true);
    log("AvahiPublisher: Starting event loop...");
    avahi_simple_poll_loop(simple_poll_);

    cleanup();
    return true;
}

} // namespace gasoline
