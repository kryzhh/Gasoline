#pragma once

// Test-only declarations. These symbols exist only in the separately built
// test-support archive and are intentionally absent from the production header
// and production archive.
#include "core/noise/clatter_ffi/include/gasoline_clatter_ffi.h"

#include <cstddef>
#include <cstdint>

extern "C" gasoline_noise_result
gasoline_noise_test_create_initiator_fixed_ephemeral(
    const std::uint8_t* prologue,
    std::size_t prologue_len,
    const std::uint8_t* ephemeral_private,
    gasoline_noise_handle** out_handle);

extern "C" gasoline_noise_result
gasoline_noise_test_create_responder_fixed_ephemeral(
    const std::uint8_t* prologue,
    std::size_t prologue_len,
    const std::uint8_t* ephemeral_private,
    gasoline_noise_handle** out_handle);
