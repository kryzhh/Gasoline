#pragma once

namespace gasoline {
// Discovery client: How will the device automatically detect and connect
// Avahi browser + Resolve + Connect (Sending "hello") packets
class DiscoveryClient {
public:
    void start();
};

}