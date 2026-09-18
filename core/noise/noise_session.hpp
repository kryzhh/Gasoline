#pragma once

#include "core/noise/clatter_ffi/include/gasoline_clatter_ffi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace gasoline::noise {

class NoiseError : public std::runtime_error {
public:
    NoiseError(const char* operation, gasoline_noise_result result);
    gasoline_noise_result result() const noexcept { return result_; }

private:
    gasoline_noise_result result_;
};

// Move-only owner for the sealed Noise_NN_25519_ChaChaPoly_BLAKE2s C handle.
// It intentionally exposes no native handle, key, nonce, or algorithm selection.
class NoiseSession final {
public:
    static NoiseSession initiator(std::string_view prologue);
    static NoiseSession responder(std::string_view prologue);

    ~NoiseSession();
    NoiseSession(NoiseSession&& other) noexcept;
    NoiseSession& operator=(NoiseSession&& other) noexcept;

    NoiseSession(const NoiseSession&) = delete;
    NoiseSession& operator=(const NoiseSession&) = delete;

    std::vector<std::uint8_t> write_handshake(
        const std::vector<std::uint8_t>& payload = {});
    std::vector<std::uint8_t> read_handshake(
        const std::vector<std::uint8_t>& message);
    bool handshake_complete() const;
    std::array<std::uint8_t, GASOLINE_NOISE_HANDSHAKE_HASH_SIZE>
    handshake_hash() const;
    void enter_transport();

    std::vector<std::uint8_t> encrypt(
        const std::vector<std::uint8_t>& plaintext);
    std::vector<std::uint8_t> decrypt(
        const std::vector<std::uint8_t>& ciphertext);
    void rekey_sender();
    void rekey_receiver();

private:
    explicit NoiseSession(gasoline_noise_handle* handle) noexcept : handle_(handle) {}
    static void check(const char* operation, gasoline_noise_result result);

    gasoline_noise_handle* handle_ = nullptr;
};

}  // namespace gasoline::noise

