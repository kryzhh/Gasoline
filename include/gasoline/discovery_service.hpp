#pragma once

#include <memory>

namespace gasoline {

class IDiscoveryPublisher;

class DiscoveryService {
public:
    DiscoveryService();
    explicit DiscoveryService(std::unique_ptr<IDiscoveryPublisher> publisher);
    ~DiscoveryService();

    void start();
    void stop();

private:
    std::unique_ptr<IDiscoveryPublisher> publisher_;
};

} // namespace gasoline