#pragma once

#include <filesystem>
#include <string>

namespace gasoline {

class DeviceIdentity {
public:
    static DeviceIdentity load_or_create();
    static DeviceIdentity load_or_create(const std::filesystem::path& identity_path);

    static std::filesystem::path default_identity_path();

    const std::string& device_id() const;

private:
    explicit DeviceIdentity(std::string device_id);

    std::string device_id_;
};

} // namespace gasoline