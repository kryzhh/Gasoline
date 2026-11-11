if(NOT DEFINED ARCHIVE)
    message(FATAL_ERROR "ARCHIVE was not supplied")
endif()

find_program(NM_EXECUTABLE NAMES llvm-nm nm)
if(NOT NM_EXECUTABLE)
    message(FATAL_ERROR "nm is required to verify the production C ABI")
endif()

execute_process(
    COMMAND "${NM_EXECUTABLE}" -A -g --defined-only "${ARCHIVE}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed: ${nm_error}")
endif()

set(expected
    gasoline_noise_copy_handshake_hash
    gasoline_noise_create_initiator
    gasoline_noise_create_responder
    gasoline_noise_destroy
    gasoline_noise_enter_transport
    gasoline_noise_handshake_is_complete
    gasoline_noise_handshake_read
    gasoline_noise_handshake_write
    gasoline_noise_rekey_receiver
    gasoline_noise_rekey_sender
    gasoline_noise_transport_decrypt
    gasoline_noise_transport_encrypt)

# A Rust staticlib also contains compiler-builtins and Rust runtime objects.
# Inspect only the object emitted for this wrapper crate, then exclude the
# small, known set of Rust runtime symbols emitted into that object. This makes
# the remaining set a true allowlist: an unrelated #[no_mangle] wrapper symbol
# cannot evade the check by choosing a different prefix.
string(REPLACE "\n" ";" symbol_lines "${symbols}")
set(exported)
foreach(line IN LISTS symbol_lines)
    if(line MATCHES ":gasoline_clatter_ffi-[^:]*\\.rcgu\\.o:")
        string(REGEX REPLACE "^.*[ \t]" "" symbol "${line}")
        if(symbol STREQUAL "DW.ref.rust_eh_personality" OR
           symbol STREQUAL "rust_eh_personality" OR
           symbol MATCHES "^_RNvNt.*_3std9panicking11EMPTY_PANIC$" OR
           symbol MATCHES "^_RNvNtNtNtNt.*_3std3sys4args4unix3imp15ARGV_INIT_ARRAY$")
            continue()
        endif()
        list(APPEND exported "${symbol}")
    endif()
endforeach()

list(REMOVE_DUPLICATES exported)
list(SORT exported)
list(SORT expected)
if(NOT exported STREQUAL expected)
    message(FATAL_ERROR
        "Production wrapper C ABI differs from its sealed whitelist.\n"
        "Expected: ${expected}\nActual: ${exported}")
endif()
