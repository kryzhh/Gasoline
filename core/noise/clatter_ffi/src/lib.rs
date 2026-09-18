use clatter::bytearray::ByteArray;
use clatter::crypto::cipher::ChaChaPoly;
use clatter::crypto::dh::X25519;
use clatter::crypto::hash::Blake2s;
use clatter::error::{HandshakeError, TransportError};
use clatter::handshakepattern::noise_nn;
use clatter::transportstate::TransportState;
use clatter::{Handshaker, NqHandshake};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::sync::Mutex;

const MAX_MESSAGE: usize = 65_535;
const TAG_SIZE: usize = 16;
const MAX_PLAINTEXT: usize = MAX_MESSAGE - TAG_SIZE;
const HASH_SIZE: usize = 32;
const MAX_PROLOGUE: usize = 4_096;

type Handshake = NqHandshake<X25519, ChaChaPoly, Blake2s>;
type Transport = TransportState<ChaChaPoly, Blake2s>;

enum SessionState {
    Handshake(Handshake),
    Transport(Transport),
    Failed,
}

#[repr(C)]
pub struct gasoline_noise_handle {
    state: Mutex<SessionState>,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum gasoline_noise_result {
    Ok = 0,
    InvalidArgument = 1,
    InvalidState = 2,
    BufferTooSmall = 3,
    Oversized = 4,
    CryptoError = 5,
    Panic = 6,
    InternalError = 7,
}

type FfiResult<T = ()> = Result<T, gasoline_noise_result>;

fn guard<F>(f: F) -> gasoline_noise_result
where
    F: FnOnce() -> FfiResult<()>,
{
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => gasoline_noise_result::Ok,
        Ok(Err(error)) => error,
        Err(_) => gasoline_noise_result::Panic,
    }
}

fn validate_const_buffer(data: *const u8, len: usize) -> FfiResult<()> {
    if len != 0 && (data.is_null() || (data as usize).checked_add(len).is_none()) {
        return Err(gasoline_noise_result::InvalidArgument);
    }
    Ok(())
}

fn validate_mut_buffer(data: *mut u8, len: usize) -> FfiResult<()> {
    if len != 0 && (data.is_null() || (data as usize).checked_add(len).is_none()) {
        return Err(gasoline_noise_result::InvalidArgument);
    }
    Ok(())
}

fn validate_output_parameter<T>(data: *mut T) -> FfiResult<()> {
    let address = data as usize;
    if data.is_null()
        || address % std::mem::align_of::<T>() != 0
        || address.checked_add(std::mem::size_of::<T>()).is_none()
    {
        return Err(gasoline_noise_result::InvalidArgument);
    }
    Ok(())
}

fn checked_const_slice<'a>(data: *const u8, len: usize) -> FfiResult<&'a [u8]> {
    if len == 0 {
        return Ok(&[]);
    }
    validate_const_buffer(data, len)?;
    // SAFETY: The caller supplies readable storage for len bytes. Address wrap is
    // rejected above, and all public operations impose a small maximum first.
    Ok(unsafe { slice::from_raw_parts(data, len) })
}

fn checked_mut_slice<'a>(data: *mut u8, len: usize) -> FfiResult<&'a mut [u8]> {
    if len == 0 {
        return Ok(&mut []);
    }
    validate_mut_buffer(data, len)?;
    // SAFETY: The caller supplies uniquely borrowed writable storage for len bytes.
    // Address wrap is rejected above, and lengths are bounded by public operations.
    Ok(unsafe { slice::from_raw_parts_mut(data, len) })
}

fn ranges_overlap(a: *const u8, a_len: usize, b: *mut u8, b_len: usize) -> bool {
    if a_len == 0 || b_len == 0 {
        return false;
    }
    let Some(a_end) = (a as usize).checked_add(a_len) else {
        return true;
    };
    let Some(b_end) = (b as usize).checked_add(b_len) else {
        return true;
    };
    (a as usize) < b_end && (b as usize) < a_end
}

fn handle_ref<'a>(handle: *mut gasoline_noise_handle) -> FfiResult<&'a gasoline_noise_handle> {
    if handle.is_null() {
        return Err(gasoline_noise_result::InvalidArgument);
    }
    // SAFETY: Handles are created and destroyed only by this library. The C API
    // requires destruction not to race an operation on the same handle.
    Ok(unsafe { &*handle })
}

fn create(
    prologue: *const u8,
    prologue_len: usize,
    initiator: bool,
    out_handle: *mut *mut gasoline_noise_handle,
) -> FfiResult<()> {
    if out_handle.is_null() {
        return Err(gasoline_noise_result::InvalidArgument);
    }
    // Always clear the result before any fallible operation.
    unsafe { *out_handle = ptr::null_mut() };
    if prologue_len > MAX_PROLOGUE {
        return Err(gasoline_noise_result::Oversized);
    }
    let prologue = checked_const_slice(prologue, prologue_len)?;
    let handshake = Handshake::new(noise_nn(), prologue, initiator, None, None, None, None)
        .map_err(map_handshake_error)?;
    let handle = Box::new(gasoline_noise_handle {
        state: Mutex::new(SessionState::Handshake(handshake)),
    });
    unsafe { *out_handle = Box::into_raw(handle) };
    Ok(())
}

fn map_handshake_error(error: HandshakeError) -> gasoline_noise_result {
    match error {
        HandshakeError::InvalidState | HandshakeError::ErrorState => {
            gasoline_noise_result::InvalidState
        }
        HandshakeError::BufferTooSmall => gasoline_noise_result::BufferTooSmall,
        HandshakeError::InvalidMessage => gasoline_noise_result::CryptoError,
        _ => gasoline_noise_result::CryptoError,
    }
}

fn map_transport_error(error: TransportError) -> gasoline_noise_result {
    match error {
        TransportError::BufferTooSmall => gasoline_noise_result::BufferTooSmall,
        TransportError::TooShort => gasoline_noise_result::InvalidArgument,
        TransportError::OneWayViolation => gasoline_noise_result::InvalidState,
        TransportError::Cipher(_) => gasoline_noise_result::CryptoError,
    }
}

#[no_mangle]
pub extern "C" fn gasoline_noise_create_initiator(
    prologue: *const u8,
    prologue_len: usize,
    out_handle: *mut *mut gasoline_noise_handle,
) -> gasoline_noise_result {
    guard(|| create(prologue, prologue_len, true, out_handle))
}

#[no_mangle]
pub extern "C" fn gasoline_noise_create_responder(
    prologue: *const u8,
    prologue_len: usize,
    out_handle: *mut *mut gasoline_noise_handle,
) -> gasoline_noise_result {
    guard(|| create(prologue, prologue_len, false, out_handle))
}

#[no_mangle]
pub extern "C" fn gasoline_noise_handshake_write(
    handle: *mut gasoline_noise_handle,
    payload: *const u8,
    payload_len: usize,
    output: *mut u8,
    output_capacity: usize,
    output_len: *mut usize,
) -> gasoline_noise_result {
    guard(|| {
        validate_output_parameter(output_len)?;
        validate_const_buffer(payload, payload_len)?;
        validate_mut_buffer(output, output_capacity)?;
        if ranges_overlap(payload, payload_len, output, output_capacity)
            || ranges_overlap(
                payload,
                payload_len,
                output_len.cast::<u8>(),
                std::mem::size_of::<usize>(),
            )
            || ranges_overlap(
                output,
                output_capacity,
                output_len.cast::<u8>(),
                std::mem::size_of::<usize>(),
            )
        {
            return Err(gasoline_noise_result::InvalidArgument);
        }
        unsafe { *output_len = 0 };
        if payload_len > MAX_MESSAGE || output_capacity > MAX_MESSAGE {
            return Err(gasoline_noise_result::Oversized);
        }
        let handle = handle_ref(handle)?;
        let mut state = handle
            .state
            .lock()
            .map_err(|_| gasoline_noise_result::InternalError)?;
        let SessionState::Handshake(handshake) = &mut *state else {
            return Err(gasoline_noise_result::InvalidState);
        };
        let overhead = handshake
            .get_next_message_overhead()
            .map_err(map_handshake_error)?;
        let required = payload_len
            .checked_add(overhead)
            .ok_or(gasoline_noise_result::Oversized)?;
        if required > MAX_MESSAGE {
            return Err(gasoline_noise_result::Oversized);
        }
        if output_capacity < required {
            return Err(gasoline_noise_result::BufferTooSmall);
        }
        let payload = checked_const_slice(payload, payload_len)?;
        let output = checked_mut_slice(output, output_capacity)?;
        let written = handshake
            .write_message(payload, output)
            .map_err(map_handshake_error)?;
        unsafe { *output_len = written };
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn gasoline_noise_handshake_read(
    handle: *mut gasoline_noise_handle,
    message: *const u8,
    message_len: usize,
    payload_output: *mut u8,
    payload_capacity: usize,
    payload_len: *mut usize,
) -> gasoline_noise_result {
    guard(|| {
        validate_output_parameter(payload_len)?;
        validate_const_buffer(message, message_len)?;
        validate_mut_buffer(payload_output, payload_capacity)?;
        if ranges_overlap(message, message_len, payload_output, payload_capacity)
            || ranges_overlap(
                message,
                message_len,
                payload_len.cast::<u8>(),
                std::mem::size_of::<usize>(),
            )
            || ranges_overlap(
                payload_output,
                payload_capacity,
                payload_len.cast::<u8>(),
                std::mem::size_of::<usize>(),
            )
        {
            return Err(gasoline_noise_result::InvalidArgument);
        }
        unsafe { *payload_len = 0 };
        if message_len > MAX_MESSAGE || payload_capacity > MAX_MESSAGE {
            return Err(gasoline_noise_result::Oversized);
        }
        let handle = handle_ref(handle)?;
        let mut state = handle
            .state
            .lock()
            .map_err(|_| gasoline_noise_result::InternalError)?;
        let SessionState::Handshake(handshake) = &mut *state else {
            return Err(gasoline_noise_result::InvalidState);
        };
        let overhead = handshake
            .get_next_message_overhead()
            .map_err(map_handshake_error)?;
        if message_len < overhead {
            return Err(gasoline_noise_result::CryptoError);
        }
        let required = message_len - overhead;
        if payload_capacity < required {
            return Err(gasoline_noise_result::BufferTooSmall);
        }
        let message = checked_const_slice(message, message_len)?;
        let output = checked_mut_slice(payload_output, payload_capacity)?;
        let written = handshake
            .read_message(message, output)
            .map_err(map_handshake_error)?;
        unsafe { *payload_len = written };
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn gasoline_noise_handshake_is_complete(
    handle: *mut gasoline_noise_handle,
    complete: *mut u8,
) -> gasoline_noise_result {
    guard(|| {
        if complete.is_null() {
            return Err(gasoline_noise_result::InvalidArgument);
        }
        unsafe { *complete = 0 };
        let handle = handle_ref(handle)?;
        let state = handle
            .state
            .lock()
            .map_err(|_| gasoline_noise_result::InternalError)?;
        match &*state {
            SessionState::Handshake(handshake) => unsafe {
                *complete = u8::from(handshake.is_finished())
            },
            SessionState::Transport(_) => unsafe { *complete = 1 },
            SessionState::Failed => return Err(gasoline_noise_result::InvalidState),
        }
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn gasoline_noise_copy_handshake_hash(
    handle: *mut gasoline_noise_handle,
    output: *mut u8,
    output_capacity: usize,
) -> gasoline_noise_result {
    guard(|| {
        if output_capacity < HASH_SIZE {
            return Err(gasoline_noise_result::BufferTooSmall);
        }
        let output = checked_mut_slice(output, HASH_SIZE)?;
        let handle = handle_ref(handle)?;
        let state = handle
            .state
            .lock()
            .map_err(|_| gasoline_noise_result::InternalError)?;
        match &*state {
            SessionState::Handshake(handshake) if handshake.is_finished() => {
                output.copy_from_slice(handshake.get_state().get_hash().as_slice());
            }
            SessionState::Transport(transport) => {
                output.copy_from_slice(transport.get_handshake_hash().as_slice());
            }
            _ => return Err(gasoline_noise_result::InvalidState),
        }
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn gasoline_noise_enter_transport(
    handle: *mut gasoline_noise_handle,
) -> gasoline_noise_result {
    guard(|| {
        let handle = handle_ref(handle)?;
        let mut state = handle
            .state
            .lock()
            .map_err(|_| gasoline_noise_result::InternalError)?;
        let old = std::mem::replace(&mut *state, SessionState::Failed);
        let SessionState::Handshake(handshake) = old else {
            *state = old;
            return Err(gasoline_noise_result::InvalidState);
        };
        if !handshake.is_finished() {
            *state = SessionState::Handshake(handshake);
            return Err(gasoline_noise_result::InvalidState);
        }
        let transport = handshake.finalize().map_err(map_handshake_error)?;
        *state = SessionState::Transport(transport);
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn gasoline_noise_transport_encrypt(
    handle: *mut gasoline_noise_handle,
    plaintext: *const u8,
    plaintext_len: usize,
    output: *mut u8,
    output_capacity: usize,
    output_len: *mut usize,
) -> gasoline_noise_result {
    guard(|| {
        if output_len.is_null() {
            return Err(gasoline_noise_result::InvalidArgument);
        }
        unsafe { *output_len = 0 };
        if plaintext_len > MAX_PLAINTEXT || output_capacity > MAX_MESSAGE {
            return Err(gasoline_noise_result::Oversized);
        }
        let required = plaintext_len + TAG_SIZE;
        if output_capacity < required {
            return Err(gasoline_noise_result::BufferTooSmall);
        }
        if ranges_overlap(plaintext, plaintext_len, output, output_capacity) {
            return Err(gasoline_noise_result::InvalidArgument);
        }
        let plaintext = checked_const_slice(plaintext, plaintext_len)?;
        let output = checked_mut_slice(output, output_capacity)?;
        let handle = handle_ref(handle)?;
        let mut state = handle
            .state
            .lock()
            .map_err(|_| gasoline_noise_result::InternalError)?;
        let SessionState::Transport(transport) = &mut *state else {
            return Err(gasoline_noise_result::InvalidState);
        };
        let written = transport
            .send(plaintext, output)
            .map_err(map_transport_error)?;
        unsafe { *output_len = written };
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn gasoline_noise_transport_decrypt(
    handle: *mut gasoline_noise_handle,
    ciphertext: *const u8,
    ciphertext_len: usize,
    output: *mut u8,
    output_capacity: usize,
    output_len: *mut usize,
) -> gasoline_noise_result {
    guard(|| {
        if output_len.is_null() {
            return Err(gasoline_noise_result::InvalidArgument);
        }
        unsafe { *output_len = 0 };
        if ciphertext_len > MAX_MESSAGE || output_capacity > MAX_PLAINTEXT {
            return Err(gasoline_noise_result::Oversized);
        }
        if ciphertext_len < TAG_SIZE {
            return Err(gasoline_noise_result::InvalidArgument);
        }
        let required = ciphertext_len - TAG_SIZE;
        if output_capacity < required {
            return Err(gasoline_noise_result::BufferTooSmall);
        }
        if ranges_overlap(ciphertext, ciphertext_len, output, output_capacity) {
            return Err(gasoline_noise_result::InvalidArgument);
        }
        let ciphertext = checked_const_slice(ciphertext, ciphertext_len)?;
        let output = checked_mut_slice(output, output_capacity)?;
        let handle = handle_ref(handle)?;
        let mut state = handle
            .state
            .lock()
            .map_err(|_| gasoline_noise_result::InternalError)?;
        let SessionState::Transport(transport) = &mut *state else {
            return Err(gasoline_noise_result::InvalidState);
        };
        match transport.receive(ciphertext, output) {
            Ok(written) => {
                unsafe { *output_len = written };
                Ok(())
            }
            Err(error) => {
                // Authentication failures are terminal: continuing would risk nonce
                // desynchronization or accidental reuse after rejected ciphertext.
                let mapped = map_transport_error(error);
                *state = SessionState::Failed;
                Err(mapped)
            }
        }
    })
}

#[no_mangle]
pub extern "C" fn gasoline_noise_rekey_sender(
    handle: *mut gasoline_noise_handle,
) -> gasoline_noise_result {
    guard(|| {
        let handle = handle_ref(handle)?;
        let mut state = handle
            .state
            .lock()
            .map_err(|_| gasoline_noise_result::InternalError)?;
        let SessionState::Transport(transport) = &mut *state else {
            return Err(gasoline_noise_result::InvalidState);
        };
        transport.rekey_sender().map_err(map_transport_error)
    })
}

#[no_mangle]
pub extern "C" fn gasoline_noise_rekey_receiver(
    handle: *mut gasoline_noise_handle,
) -> gasoline_noise_result {
    guard(|| {
        let handle = handle_ref(handle)?;
        let mut state = handle
            .state
            .lock()
            .map_err(|_| gasoline_noise_result::InternalError)?;
        let SessionState::Transport(transport) = &mut *state else {
            return Err(gasoline_noise_result::InvalidState);
        };
        transport.rekey_receiver().map_err(map_transport_error)
    })
}

#[no_mangle]
pub extern "C" fn gasoline_noise_destroy(
    handle: *mut gasoline_noise_handle,
) -> gasoline_noise_result {
    guard(|| {
        if !handle.is_null() {
            // SAFETY: The pointer was returned by create, is uniquely owned by the
            // caller, and the API contract forbids concurrent use during destruction.
            unsafe { drop(Box::from_raw(handle)) };
        }
        Ok(())
    })
}

#[cfg(feature = "test-support")]
fn create_with_fixed_ephemeral(
    prologue: *const u8,
    prologue_len: usize,
    ephemeral_private: *const u8,
    initiator: bool,
    out_handle: *mut *mut gasoline_noise_handle,
) -> FfiResult<()> {
    use clatter::traits::Dh;
    use clatter::KeyPair;

    if out_handle.is_null() || ephemeral_private.is_null() {
        return Err(gasoline_noise_result::InvalidArgument);
    }
    unsafe { *out_handle = ptr::null_mut() };
    if prologue_len > MAX_PROLOGUE {
        return Err(gasoline_noise_result::Oversized);
    }
    let prologue = checked_const_slice(prologue, prologue_len)?;
    let private_bytes = checked_const_slice(ephemeral_private, 32)?;
    let private = <X25519 as Dh>::PrivateKey::from_slice(private_bytes);
    let public = X25519::pubkey(&private);
    let ephemeral = KeyPair::new(public, private);
    let handshake = Handshake::new(
        noise_nn(),
        prologue,
        initiator,
        None,
        Some(ephemeral),
        None,
        None,
    )
    .map_err(map_handshake_error)?;
    let handle = Box::new(gasoline_noise_handle {
        state: Mutex::new(SessionState::Handshake(handshake)),
    });
    unsafe { *out_handle = Box::into_raw(handle) };
    Ok(())
}

#[cfg(feature = "test-support")]
#[no_mangle]
pub extern "C" fn gasoline_noise_test_create_initiator_fixed_ephemeral(
    prologue: *const u8,
    prologue_len: usize,
    ephemeral_private: *const u8,
    out_handle: *mut *mut gasoline_noise_handle,
) -> gasoline_noise_result {
    guard(|| {
        create_with_fixed_ephemeral(prologue, prologue_len, ephemeral_private, true, out_handle)
    })
}

#[cfg(feature = "test-support")]
#[no_mangle]
pub extern "C" fn gasoline_noise_test_create_responder_fixed_ephemeral(
    prologue: *const u8,
    prologue_len: usize,
    ephemeral_private: *const u8,
    out_handle: *mut *mut gasoline_noise_handle,
) -> gasoline_noise_result {
    guard(|| {
        create_with_fixed_ephemeral(prologue, prologue_len, ephemeral_private, false, out_handle)
    })
}
