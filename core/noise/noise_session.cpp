#include "core/noise/noise_session.hpp"

#include <string>
#include <utility>

namespace gasoline::noise {
namespace {

const char* result_name(gasoline_noise_result result) {
    switch (result) {
        case GASOLINE_NOISE_OK: return "ok";
        case GASOLINE_NOISE_INVALID_ARGUMENT: return "invalid argument";
        case GASOLINE_NOISE_INVALID_STATE: return "invalid state";
        case GASOLINE_NOISE_BUFFER_TOO_SMALL: return "buffer too small";
        case GASOLINE_NOISE_OVERSIZED: return "oversized input";
        case GASOLINE_NOISE_CRYPTO_ERROR: return "cryptographic error";
        case GASOLINE_NOISE_PANIC: return "contained Rust panic";
        case GASOLINE_NOISE_INTERNAL_ERROR: return "internal error";
    }
    return "unknown error";
}

std::string error_message(const char* operation, gasoline_noise_result result) {
    return std::string(operation) + ": " + result_name(result);
}

const std::uint8_t* bytes(std::string_view value) {
    return reinterpret_cast<const std::uint8_t*>(value.data());
}

}  // namespace

NoiseError::NoiseError(const char* operation, gasoline_noise_result result)
    : std::runtime_error(error_message(operation, result)), result_(result) {}

void NoiseSession::check(const char* operation, gasoline_noise_result result) {
    if (result != GASOLINE_NOISE_OK) {
        throw NoiseError(operation, result);
    }
}

NoiseSession NoiseSession::initiator(std::string_view prologue) {
    gasoline_noise_handle* handle = nullptr;
    check("create initiator", gasoline_noise_create_initiator(
                                  bytes(prologue), prologue.size(), &handle));
    return NoiseSession(handle);
}

NoiseSession NoiseSession::responder(std::string_view prologue) {
    gasoline_noise_handle* handle = nullptr;
    check("create responder", gasoline_noise_create_responder(
                                  bytes(prologue), prologue.size(), &handle));
    return NoiseSession(handle);
}

NoiseSession::~NoiseSession() {
    if (handle_ != nullptr) {
        (void)gasoline_noise_destroy(handle_);
    }
}

NoiseSession::NoiseSession(NoiseSession&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

NoiseSession& NoiseSession::operator=(NoiseSession&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            (void)gasoline_noise_destroy(handle_);
        }
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

std::vector<std::uint8_t> NoiseSession::write_handshake(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() > GASOLINE_NOISE_MAX_MESSAGE_SIZE) {
        throw NoiseError("write handshake", GASOLINE_NOISE_OVERSIZED);
    }
    std::vector<std::uint8_t> output(GASOLINE_NOISE_MAX_MESSAGE_SIZE);
    std::size_t written = 0;
    check("write handshake", gasoline_noise_handshake_write(
                                 handle_, payload.data(), payload.size(), output.data(),
                                 output.size(), &written));
    output.resize(written);
    return output;
}

std::vector<std::uint8_t> NoiseSession::read_handshake(
    const std::vector<std::uint8_t>& message) {
    if (message.size() > GASOLINE_NOISE_MAX_MESSAGE_SIZE) {
        throw NoiseError("read handshake", GASOLINE_NOISE_OVERSIZED);
    }
    std::vector<std::uint8_t> output(message.size());
    std::size_t written = 0;
    check("read handshake", gasoline_noise_handshake_read(
                                handle_, message.data(), message.size(), output.data(),
                                output.size(), &written));
    output.resize(written);
    return output;
}

bool NoiseSession::handshake_complete() const {
    std::uint8_t complete = 0;
    check("check handshake", gasoline_noise_handshake_is_complete(handle_, &complete));
    return complete != 0;
}

std::array<std::uint8_t, GASOLINE_NOISE_HANDSHAKE_HASH_SIZE>
NoiseSession::handshake_hash() const {
    std::array<std::uint8_t, GASOLINE_NOISE_HANDSHAKE_HASH_SIZE> hash{};
    check("copy handshake hash", gasoline_noise_copy_handshake_hash(
                                     handle_, hash.data(), hash.size()));
    return hash;
}

void NoiseSession::enter_transport() {
    check("enter transport", gasoline_noise_enter_transport(handle_));
}

std::vector<std::uint8_t> NoiseSession::encrypt(
    const std::vector<std::uint8_t>& plaintext) {
    if (plaintext.size() > GASOLINE_NOISE_MAX_TRANSPORT_PLAINTEXT) {
        throw NoiseError("encrypt", GASOLINE_NOISE_OVERSIZED);
    }
    std::vector<std::uint8_t> output(plaintext.size() + GASOLINE_NOISE_TAG_SIZE);
    std::size_t written = 0;
    check("encrypt", gasoline_noise_transport_encrypt(
                         handle_, plaintext.data(), plaintext.size(), output.data(),
                         output.size(), &written));
    output.resize(written);
    return output;
}

std::vector<std::uint8_t> NoiseSession::decrypt(
    const std::vector<std::uint8_t>& ciphertext) {
    if (ciphertext.size() > GASOLINE_NOISE_MAX_MESSAGE_SIZE) {
        throw NoiseError("decrypt", GASOLINE_NOISE_OVERSIZED);
    }
    const std::size_t maximum_plaintext = ciphertext.size() >= GASOLINE_NOISE_TAG_SIZE
                                              ? ciphertext.size() - GASOLINE_NOISE_TAG_SIZE
                                              : 0;
    std::vector<std::uint8_t> output(maximum_plaintext);
    std::size_t written = 0;
    check("decrypt", gasoline_noise_transport_decrypt(
                         handle_, ciphertext.data(), ciphertext.size(), output.data(),
                         output.size(), &written));
    output.resize(written);
    return output;
}

void NoiseSession::rekey_sender() {
    check("rekey sender", gasoline_noise_rekey_sender(handle_));
}

void NoiseSession::rekey_receiver() {
    check("rekey receiver", gasoline_noise_rekey_receiver(handle_));
}

}  // namespace gasoline::noise
