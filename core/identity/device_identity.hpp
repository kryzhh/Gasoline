#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <sodium.h>

namespace gasoline {

class DeviceIdentity {
public:
    using PublicKey = std::array<unsigned char, crypto_sign_PUBLICKEYBYTES>;
    using Signature = std::array<unsigned char, crypto_sign_BYTES>;

    ~DeviceIdentity();
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;

    static DeviceIdentity load_or_create();
    static DeviceIdentity load_or_create(const std::filesystem::path& identity_path);

    static std::filesystem::path default_identity_path();

    const std::string& device_id() const;
    const PublicKey& public_key() const;

#ifdef GASOLINE_IDENTITY_TESTING
    // Persistence tests need to prove that the loaded secret still corresponds
    // to public_key(). This arbitrary-byte signing hook is absent from every
    // production target.
    Signature sign_bytes_for_test(std::string_view bytes) const;
#endif

private:
    // The future authenticator will receive a structured, domain-separated
    // signing operation here once its transcript format exists. Deliberately do
    // not expose a generic production signing oracle in the meantime.
    using PrivateKey = std::array<unsigned char, crypto_sign_SECRETKEYBYTES>;
    DeviceIdentity(std::string device_id, const PublicKey& public_key, const PrivateKey& private_key);

    std::string device_id_;
    PublicKey public_key_;
    PrivateKey private_key_;
};

} // namespace gasoline
