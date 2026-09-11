#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <sodium.h>

namespace gasoline {

class DeviceIdentity {
public:
    using PublicKey = std::array<unsigned char, crypto_sign_PUBLICKEYBYTES>;
    using PrivateKey = std::array<unsigned char, crypto_sign_SECRETKEYBYTES>;

    ~DeviceIdentity();
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;

    static DeviceIdentity load_or_create();
    static DeviceIdentity load_or_create(const std::filesystem::path& identity_path);

    static std::filesystem::path default_identity_path();

    const std::string& device_id() const;
    const PublicKey& public_key() const;
    const PrivateKey& private_key() const;

private:
    DeviceIdentity(std::string device_id, const PublicKey& public_key, const PrivateKey& private_key);

    std::string device_id_;
    PublicKey public_key_;
    PrivateKey private_key_;
};

} // namespace gasoline
