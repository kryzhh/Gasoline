#ifndef GASOLINE_CLATTER_FFI_H
#define GASOLINE_CLATTER_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gasoline_noise_handle gasoline_noise_handle;

typedef enum gasoline_noise_result {
    GASOLINE_NOISE_OK = 0,
    GASOLINE_NOISE_INVALID_ARGUMENT = 1,
    GASOLINE_NOISE_INVALID_STATE = 2,
    GASOLINE_NOISE_BUFFER_TOO_SMALL = 3,
    GASOLINE_NOISE_OVERSIZED = 4,
    GASOLINE_NOISE_CRYPTO_ERROR = 5,
    GASOLINE_NOISE_PANIC = 6,
    GASOLINE_NOISE_INTERNAL_ERROR = 7
} gasoline_noise_result;

enum {
    GASOLINE_NOISE_HANDSHAKE_HASH_SIZE = 32,
    GASOLINE_NOISE_TAG_SIZE = 16,
    GASOLINE_NOISE_MAX_MESSAGE_SIZE = 65535,
    GASOLINE_NOISE_MAX_TRANSPORT_PLAINTEXT = 65519,
    GASOLINE_NOISE_MAX_PROLOGUE_SIZE = 4096
};

/* Every handle is permanently bound to Noise_NN_25519_ChaChaPoly_BLAKE2s. */
gasoline_noise_result gasoline_noise_create_initiator(
    const uint8_t* prologue,
    size_t prologue_len,
    gasoline_noise_handle** out_handle);

gasoline_noise_result gasoline_noise_create_responder(
    const uint8_t* prologue,
    size_t prologue_len,
    gasoline_noise_handle** out_handle);

gasoline_noise_result gasoline_noise_handshake_write(
    gasoline_noise_handle* handle,
    const uint8_t* payload,
    size_t payload_len,
    uint8_t* output,
    size_t output_capacity,
    size_t* output_len);

gasoline_noise_result gasoline_noise_handshake_read(
    gasoline_noise_handle* handle,
    const uint8_t* message,
    size_t message_len,
    uint8_t* payload_output,
    size_t payload_capacity,
    size_t* payload_len);

gasoline_noise_result gasoline_noise_handshake_is_complete(
    gasoline_noise_handle* handle,
    uint8_t* complete);

gasoline_noise_result gasoline_noise_copy_handshake_hash(
    gasoline_noise_handle* handle,
    uint8_t* output,
    size_t output_capacity);

gasoline_noise_result gasoline_noise_enter_transport(
    gasoline_noise_handle* handle);

gasoline_noise_result gasoline_noise_transport_encrypt(
    gasoline_noise_handle* handle,
    const uint8_t* plaintext,
    size_t plaintext_len,
    uint8_t* output,
    size_t output_capacity,
    size_t* output_len);

gasoline_noise_result gasoline_noise_transport_decrypt(
    gasoline_noise_handle* handle,
    const uint8_t* ciphertext,
    size_t ciphertext_len,
    uint8_t* output,
    size_t output_capacity,
    size_t* output_len);

gasoline_noise_result gasoline_noise_rekey_sender(gasoline_noise_handle* handle);
gasoline_noise_result gasoline_noise_rekey_receiver(gasoline_noise_handle* handle);

/* Destroy must not race another operation on the same handle. NULL is accepted. */
gasoline_noise_result gasoline_noise_destroy(gasoline_noise_handle* handle);

#ifdef __cplusplus
}
#endif

#endif

