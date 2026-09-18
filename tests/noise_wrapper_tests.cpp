#include "core/noise/noise_session.hpp"
#include "tests/clatter_ffi_test.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define REQUIRE(expression)                                                      \
    do {                                                                         \
        if (!(expression)) {                                                     \
            throw std::runtime_error("requirement failed: " #expression);       \
        }                                                                        \
    } while (false)

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes hex(std::string_view value) {
    auto nibble = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
        throw std::invalid_argument("non-hex character");
    };
    if ((value.size() % 2) != 0) throw std::invalid_argument("odd hex string");
    Bytes output(value.size() / 2);
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = static_cast<std::uint8_t>((nibble(value[2 * i]) << 4) |
                                              nibble(value[2 * i + 1]));
    }
    return output;
}

struct CHandle {
    gasoline_noise_handle* value = nullptr;

    ~CHandle() {
        if (value != nullptr) {
            if (gasoline_noise_destroy(value) != GASOLINE_NOISE_OK) {
                std::terminate();
            }
        }
    }
    CHandle() = default;
    CHandle(const CHandle&) = delete;
    CHandle& operator=(const CHandle&) = delete;
};

Bytes handshake_write(gasoline_noise_handle* handle, const Bytes& payload) {
    Bytes output(GASOLINE_NOISE_MAX_MESSAGE_SIZE);
    std::size_t written = 0;
    REQUIRE(gasoline_noise_handshake_write(handle, payload.data(), payload.size(),
                                           output.data(), output.size(), &written) ==
           GASOLINE_NOISE_OK);
    output.resize(written);
    return output;
}

Bytes handshake_read(gasoline_noise_handle* handle, const Bytes& message) {
    Bytes output(message.size());
    std::size_t written = 0;
    REQUIRE(gasoline_noise_handshake_read(handle, message.data(), message.size(),
                                          output.data(), output.size(), &written) ==
           GASOLINE_NOISE_OK);
    output.resize(written);
    return output;
}

Bytes transport_encrypt(gasoline_noise_handle* handle, const Bytes& plaintext) {
    Bytes output(plaintext.size() + GASOLINE_NOISE_TAG_SIZE);
    std::size_t written = 0;
    REQUIRE(gasoline_noise_transport_encrypt(handle, plaintext.data(), plaintext.size(),
                                             output.data(), output.size(), &written) ==
           GASOLINE_NOISE_OK);
    output.resize(written);
    return output;
}

Bytes transport_decrypt(gasoline_noise_handle* handle, const Bytes& ciphertext) {
    Bytes output(ciphertext.size() - GASOLINE_NOISE_TAG_SIZE);
    std::size_t written = 0;
    REQUIRE(gasoline_noise_transport_decrypt(handle, ciphertext.data(), ciphertext.size(),
                                             output.data(), output.size(), &written) ==
           GASOLINE_NOISE_OK);
    output.resize(written);
    return output;
}

void complete_handshake(gasoline::noise::NoiseSession& initiator,
                        gasoline::noise::NoiseSession& responder) {
    auto first = initiator.write_handshake();
    REQUIRE(responder.read_handshake(first).empty());
    auto second = responder.write_handshake();
    REQUIRE(initiator.read_handshake(second).empty());
    REQUIRE(initiator.handshake_complete());
    REQUIRE(responder.handshake_complete());
    REQUIRE(initiator.handshake_hash() == responder.handshake_hash());
    initiator.enter_transport();
    responder.enter_transport();
}

void require_decrypt_failure(gasoline::noise::NoiseSession& receiver,
                             const Bytes& ciphertext) {
    try {
        (void)receiver.decrypt(ciphertext);
        REQUIRE(false && "ciphertext unexpectedly decrypted");
    } catch (const gasoline::noise::NoiseError& error) {
        REQUIRE(error.result() == GASOLINE_NOISE_CRYPTO_ERROR);
    }
}

void test_official_vector() {
    const Bytes prologue = hex("4a6f686e2047616c74");
    const Bytes initiator_ephemeral =
        hex("893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a");
    const Bytes responder_ephemeral =
        hex("bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b");
    const Bytes expected_hash =
        hex("a621e3943a29c1d984b43727697fbec096107d0b569031ac7e0f1131de19f4f4");

    CHandle initiator;
    CHandle responder;
    REQUIRE(gasoline_noise_test_create_initiator_fixed_ephemeral(
               prologue.data(), prologue.size(), initiator_ephemeral.data(),
               &initiator.value) == GASOLINE_NOISE_OK);
    REQUIRE(gasoline_noise_test_create_responder_fixed_ephemeral(
               prologue.data(), prologue.size(), responder_ephemeral.data(),
               &responder.value) == GASOLINE_NOISE_OK);

    const Bytes first_payload = hex("4c756477696720766f6e204d69736573");
    const Bytes first_expected = hex(
        "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c794"
        "44c756477696720766f6e204d69736573");
    const Bytes first = handshake_write(initiator.value, first_payload);
    REQUIRE(first == first_expected);
    REQUIRE(handshake_read(responder.value, first) == first_payload);

    const Bytes second_payload = hex("4d757272617920526f746862617264");
    const Bytes second_expected = hex(
        "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843"
        "ff34a6759d06e7733c83aeb5556c15bc762b664b3ba0556b1e7eaea4168bb6");
    const Bytes second = handshake_write(responder.value, second_payload);
    REQUIRE(second == second_expected);
    REQUIRE(handshake_read(initiator.value, second) == second_payload);

    std::array<std::uint8_t, GASOLINE_NOISE_HANDSHAKE_HASH_SIZE> initiator_hash{};
    std::array<std::uint8_t, GASOLINE_NOISE_HANDSHAKE_HASH_SIZE> responder_hash{};
    REQUIRE(gasoline_noise_copy_handshake_hash(initiator.value, initiator_hash.data(),
                                               initiator_hash.size()) == GASOLINE_NOISE_OK);
    REQUIRE(gasoline_noise_copy_handshake_hash(responder.value, responder_hash.data(),
                                               responder_hash.size()) == GASOLINE_NOISE_OK);
    REQUIRE(std::equal(expected_hash.begin(), expected_hash.end(), initiator_hash.begin()));
    REQUIRE(initiator_hash == responder_hash);

    REQUIRE(gasoline_noise_enter_transport(initiator.value) == GASOLINE_NOISE_OK);
    REQUIRE(gasoline_noise_enter_transport(responder.value) == GASOLINE_NOISE_OK);

    const std::array<std::pair<std::string_view, std::string_view>, 4> transport_vectors{{
        {"462e20412e20486179656b", "79285da88da3535f52b07b70006c85706de7ddb1fd3dddac995b7e"},
        {"4361726c204d656e676572", "ffdad3a7f0db4c39077f223659c5c1d107666405566ecdf4ab53bf"},
        {"4a65616e2d426170746973746520536179", "2b9801f5084b9a7e9df57382fb4af099a63cd8ff97bc3284c4c5f28994be58ae46"},
        {"457567656e2042f6686d20766f6e2042617765726b", "6c94a97c5de175c870fb9e8d5c50c59d20752b0695baf24e151011ee46a184a65b444e9d97"},
    }};

    for (std::size_t i = 0; i < transport_vectors.size(); ++i) {
        const Bytes plaintext = hex(transport_vectors[i].first);
        const Bytes expected = hex(transport_vectors[i].second);
        auto* sender = (i % 2 == 0) ? initiator.value : responder.value;
        auto* receiver = (i % 2 == 0) ? responder.value : initiator.value;
        const Bytes ciphertext = transport_encrypt(sender, plaintext);
        REQUIRE(ciphertext == expected);
        REQUIRE(transport_decrypt(receiver, ciphertext) == plaintext);
    }
}

void test_cpp_raii_and_rekey() {
    using gasoline::noise::NoiseSession;
    auto initiator = NoiseSession::initiator("Gasoline wrapper test");
    auto responder = NoiseSession::responder("Gasoline wrapper test");
    complete_handshake(initiator, responder);

    const Bytes first{0, 1, 2, 3, 4, 255};
    REQUIRE(responder.decrypt(initiator.encrypt(first)) == first);
    const Bytes reply{'r', 'e', 'p', 'l', 'y'};
    REQUIRE(initiator.decrypt(responder.encrypt(reply)) == reply);

    const Bytes after_rekey{'n', 'e', 'w', ' ', 'k', 'e', 'y'};

    // Sender-only rekey must change the initiator -> responder key. If the
    // wrapper accidentally rekeys the receiver instead, this decrypt succeeds.
    auto sender_only = NoiseSession::initiator("sender-only rekey test");
    auto sender_only_peer = NoiseSession::responder("sender-only rekey test");
    complete_handshake(sender_only, sender_only_peer);
    sender_only.rekey_sender();
    require_decrypt_failure(sender_only_peer, sender_only.encrypt(after_rekey));

    // Receiver-only rekey independently proves the responder's inbound mapping.
    auto receiver_only = NoiseSession::initiator("receiver-only rekey test");
    auto receiver_only_peer = NoiseSession::responder("receiver-only rekey test");
    complete_handshake(receiver_only, receiver_only_peer);
    receiver_only_peer.rekey_receiver();
    require_decrypt_failure(receiver_only_peer, receiver_only.encrypt(after_rekey));

    // Matching sender/receiver rekeys must restore interoperability.
    auto matched = NoiseSession::initiator("matched rekey test");
    auto matched_peer = NoiseSession::responder("matched rekey test");
    complete_handshake(matched, matched_peer);
    matched.rekey_sender();
    matched_peer.rekey_receiver();
    REQUIRE(matched_peer.decrypt(matched.encrypt(after_rekey)) == after_rekey);

    // Prove the reverse transport direction as well.
    matched_peer.rekey_sender();
    matched.rekey_receiver();
    REQUIRE(matched.decrypt(matched_peer.encrypt(after_rekey)) == after_rekey);

    NoiseSession moved = std::move(initiator);
    REQUIRE(responder.decrypt(moved.encrypt(Bytes{'m', 'o', 'v', 'e'})) ==
           Bytes({'m', 'o', 'v', 'e'}));
}

void test_input_validation_and_failure_destruction() {
    std::uint8_t input_byte = 0;
    std::uint8_t output_byte = 0;
    std::size_t written = 123;
    REQUIRE(gasoline_noise_destroy(nullptr) == GASOLINE_NOISE_OK);

    CHandle initiator;
    REQUIRE(gasoline_noise_create_initiator(nullptr, 0, &initiator.value) ==
           GASOLINE_NOISE_OK);
    Bytes oversized_handshake(GASOLINE_NOISE_MAX_MESSAGE_SIZE + 1ULL);
    REQUIRE(gasoline_noise_handshake_write(
               initiator.value, oversized_handshake.data(),
               oversized_handshake.size(),
               nullptr, 0, &written) == GASOLINE_NOISE_OVERSIZED);
    REQUIRE(written == 0);
    REQUIRE(gasoline_noise_handshake_write(initiator.value, nullptr, 1,
                                           &output_byte, 1,
                                           &written) ==
           GASOLINE_NOISE_INVALID_ARGUMENT);
    REQUIRE(gasoline_noise_handshake_write(initiator.value, &input_byte, 1,
                                           &output_byte, 1, nullptr) ==
           GASOLINE_NOISE_INVALID_ARGUMENT);
    REQUIRE(gasoline_noise_handshake_write(initiator.value, &input_byte, 1,
                                           nullptr, 1, &written) ==
           GASOLINE_NOISE_INVALID_ARGUMENT);
    std::array<std::uint8_t, 64> overlapping_write{};
    REQUIRE(gasoline_noise_handshake_write(
               initiator.value, overlapping_write.data(), 1,
               overlapping_write.data(), overlapping_write.size(), &written) ==
           GASOLINE_NOISE_INVALID_ARGUMENT);
    const auto* wrapping_pointer = reinterpret_cast<const std::uint8_t*>(
        std::numeric_limits<std::uintptr_t>::max() - 1);
    REQUIRE(gasoline_noise_handshake_write(initiator.value, wrapping_pointer, 4,
                                           overlapping_write.data(),
                                           overlapping_write.size(), &written) ==
           GASOLINE_NOISE_INVALID_ARGUMENT);
    REQUIRE(gasoline_noise_handshake_write(initiator.value, &input_byte, 1,
                                           &output_byte, 1,
                                           &written) ==
           GASOLINE_NOISE_BUFFER_TOO_SMALL);

    CHandle responder;
    REQUIRE(gasoline_noise_create_responder(nullptr, 0, &responder.value) ==
           GASOLINE_NOISE_OK);
    REQUIRE(gasoline_noise_handshake_read(
               responder.value, oversized_handshake.data(),
               oversized_handshake.size(),
               nullptr, 0, &written) == GASOLINE_NOISE_OVERSIZED);
    REQUIRE(gasoline_noise_handshake_read(responder.value, nullptr, 1,
                                          &output_byte, 1, &written) ==
           GASOLINE_NOISE_INVALID_ARGUMENT);
    REQUIRE(gasoline_noise_handshake_read(responder.value, &input_byte, 1,
                                          &output_byte, 1, nullptr) ==
           GASOLINE_NOISE_INVALID_ARGUMENT);
    REQUIRE(gasoline_noise_handshake_read(responder.value, &input_byte, 1,
                                          nullptr, 1, &written) ==
           GASOLINE_NOISE_INVALID_ARGUMENT);
    REQUIRE(gasoline_noise_handshake_read(responder.value, &input_byte, 1,
                                          &input_byte, 1, &written) ==
           GASOLINE_NOISE_INVALID_ARGUMENT);  // overlapping buffers are forbidden
    std::array<std::uint8_t, 1> malformed_output{};
    REQUIRE(gasoline_noise_handshake_read(responder.value, &input_byte, 1,
                                          malformed_output.data(),
                                          malformed_output.size(), &written) ==
           GASOLINE_NOISE_CRYPTO_ERROR);

    using gasoline::noise::NoiseError;
    using gasoline::noise::NoiseSession;
    auto a = NoiseSession::initiator("failure test");
    auto b = NoiseSession::responder("failure test");
    complete_handshake(a, b);
    auto ciphertext = a.encrypt(Bytes{'a', 'u', 't', 'h'});
    ciphertext.back() ^= 1;
    try {
        (void)b.decrypt(ciphertext);
        REQUIRE(false && "corrupt ciphertext was accepted");
    } catch (const NoiseError& error) {
        REQUIRE(error.result() == GASOLINE_NOISE_CRYPTO_ERROR);
    }
    try {
        (void)b.decrypt(ciphertext);
        REQUIRE(false && "terminal receive failure was reusable");
    } catch (const NoiseError& error) {
        REQUIRE(error.result() == GASOLINE_NOISE_INVALID_STATE);
    }

    Bytes oversized(GASOLINE_NOISE_MAX_TRANSPORT_PLAINTEXT + 1ULL);
    try {
        (void)a.encrypt(oversized);
        REQUIRE(false && "oversized plaintext was accepted");
    } catch (const NoiseError& error) {
        REQUIRE(error.result() == GASOLINE_NOISE_OVERSIZED);
    }
    Bytes oversized_message(GASOLINE_NOISE_MAX_MESSAGE_SIZE + 1ULL);
    try {
        (void)a.decrypt(oversized_message);
        REQUIRE(false && "oversized ciphertext was accepted");
    } catch (const NoiseError& error) {
        REQUIRE(error.result() == GASOLINE_NOISE_OVERSIZED);
    }

    auto max_sender = NoiseSession::initiator("maximum test");
    auto max_receiver = NoiseSession::responder("maximum test");
    complete_handshake(max_sender, max_receiver);
    Bytes maximum(GASOLINE_NOISE_MAX_TRANSPORT_PLAINTEXT, 0xa5);
    REQUIRE(max_receiver.decrypt(max_sender.encrypt(maximum)) == maximum);
}

}  // namespace

int main() {
    test_official_vector();
    test_cpp_raii_and_rekey();
    test_input_validation_and_failure_destruction();
    std::cout << "All Clatter wrapper tests passed\n";
    return 0;
}
