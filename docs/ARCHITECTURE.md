# Gasoline Architecture

## Overview
Gasoline focuses on:
* Local network communication (LAN-first design)
* Low resource usage
* Cross-platform compatibility
* Modular architecture
* Multi-device environments
The system allows devices to exchange information such as notifications, files, and control messages through a persistent network connection.

---

# System Components
Gasoline consists of three major components:
1. Android Client (Planned)
2. Gasoline Core
3. Platform Integration Layer

## Android Client (Planned)
The Android application is responsible for collecting system events and forwarding them to connected devices.
Responsibilities include:
* Monitoring device notifications
* Detecting incoming calls
* Sending events to the Gasoline daemon
* Receiving commands (notification replies, file transfer requests)
* Managing device discovery and pairing
* Allowing notification filtering by application (I do NOT want to see pointless SMSes or from apps I don't even use)
The Android client communicates with the Gasoline daemon using the Gasoline protocol over the local network.

---

## Gasoline Core
Gasoline Core contains the cross-platform logic used by all desktop implementations.
This includes:
* Network communication (TCP server and client handling)
* Packet serialization and parsing (JSON-based)
* Device management and registry
* Persistent local device identity
* Packet routing with handler classes
* Packet monitoring and logging
* Logging utilities
Gasoline Core is designed to be completely platform independent and must not depend on operating system specific APIs. 
All platform functionality is handled through the Platform Integration Layer.

### Core Modules
- **Networking**: Server listens on port 42666, handles incoming connections with multithreaded client handlers, sends packets with framing.
- **Protocol**: Packet parsing from JSON strings, routing system with dedicated handlers for each packet type.
- **Device**: Registry for managing connected devices with thread-safe operations.
- **Utils**: Logging functions for info/error messages, packet monitoring for RX/TX logging.

### Connection Lifecycle
TCP socket lifetime is owned by the networking connection/session layer.
Incoming accepted sockets and outgoing sockets both converge on the same connection path, which owns the receive loop, write serialization, and cleanup.
Protocol handlers only inspect packets and send protocol responses; they do not close sockets or remove registry entries.

Each Connection has a process-local, monotonically allocated session ID.
ConnectionManager lookup and DeviceRegistry state updates/removal use that ID,
not a socket descriptor that the OS may reuse. Registry snapshots contain a weak
Connection reference; application sends lock that reference and use the owning
session. There is no descriptor-based send overload or raw-send fallback.

The receive worker holds a strong Connection reference until cleanup finishes.
The destructor also cleans up connections that were never started. Shutdown is
idempotent and wakes pending reads/writes without closing the descriptor. Final
close waits for the serialized writer to exit; later sends fail without touching
the socket. Nonblocking sends use a 10-second monotonic write deadline so a peer
that stops reading cannot hold a writer indefinitely.

A session has 10 seconds from creation to complete `hello` followed by
`ping`/`pong` and reach READY, measured with `std::chrono::steady_clock`. Partial
traffic does not reset this deadline. READY sessions have no idle timeout in this
milestone. Only one valid peer hello is accepted. Repeated hello, changed packet
identity, malformed JSON, and packets before the first hello close the session.
Message handling after hello is unchanged, but messages do not complete or extend
the handshake. These checks enforce protocol order, not authentication.

Newline-delimited JSON framing is unchanged. Each frame is limited to 65,536
bytes excluding the newline, checked before appending to the receive buffer,
whether or not the terminating newline has arrived. The same limit applies to
outgoing frames. Fragmented and coalesced frames remain supported.

Duplicate arbitration and registration occur under one registry lock. The
connection initiated by the lower UUID is preferred when opposite directions
compete; a live incumbent wins same-direction ties. Replaced sessions are stopped
through their Connection handles, and their later cleanup cannot remove the new
owner's registry entry.

Local regression tests are available through `ctest --test-dir build
--output-on-failure` after a build with `BUILD_TESTING=ON`. They use Unix socket
pairs and a fixed test identity, without discovery, remote peers, or access to
the user's identity file.

### Local Control API
The Linux HTTP control API listens only on `127.0.0.1:42667` for local desktop
clients and development tools. It preserves `GET /devices`, `GET /events`, and
`POST /send`. Direct connections to the host's LAN address on this port are not
accepted. The device-to-device TCP service on port 42666 is separate.

The API sends no CORS headers; browser cross-origin access is not supported.
Loopback binding is not authentication: other local processes can still call the
API, and removing CORS does not prevent every browser-originated request. There
is no caller authentication or pairing approval in this interface yet.

---

## Platform Integration Layer
The platform layer connects the Gasoline Core with operating system features.
Separate adapters are implemented for each supported platform.

### Linux Integration
The Linux adapter provides:
* Device discovery using mDNS (Avahi)
* Service advertisement on local network
* Automatic device detection

### Windows Integration (Planned)
The Windows adapter will provide:
* Device discovery (TBD)
* Service advertisement
* Windows-specific system hooks

---

# Communication Model
Gasoline devices communicate over the local network using persistent TCP connections.
Communication uses structured packets encoded in JSON.
Each device runs a Gasoline daemon which listens for incoming connections on port 42666.
Devices connect to each other and exchange packets describing events or commands.

Packet structure:
```json
{
  "type": "hello",
  "device_id": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "payload": {
    "device_name": "Krish Phone",
    "device_type": "android"
  }
}
```

Currently implemented packet types:
* hello (device identification and registration)
* ping (keepalive)
* pong (keepalive response)

---

# Device Discovery
Gasoline uses mDNS (multicast DNS) for automatic device discovery on the local network.
Devices advertise themselves using the service name "_gasoline._tcp" on port 42666.
The discovery service allows devices to automatically find and connect to Gasoline daemons without manual IP configuration.

### Linux Implementation
Uses Avahi library for mDNS service advertisement and discovery.

---

# Device Management
Devices are managed through a centralized registry that tracks connected devices.
Each device is identified by:
- device_id: Unique identifier
- device_name: Human-readable name
- device_type: Platform type (android, linux, windows)
- socket_fd: Internal socket file descriptor

The registry provides thread-safe add/remove/list operations for device management.

## Device Identity
`DeviceIdentity` owns the persistent UUID and a long-term Ed25519 keypair. Build
dependencies now include libsodium development headers/library and pkg-config.
Libsodium supplies key generation, UUID randomness, key consistency checks, and
secret-buffer clearing; no cryptographic primitives are implemented locally.

Linux storage remains in `~/.config/gasoline`, secured to mode `0700`:

- `device_id`: UUID on the first line, `ed25519-v1` on the second, final newline.
- `device_id.keys`: a 152-byte binary record: ASCII `GASOLINE-ED25519-V1\n`
  (20 bytes), the matching ASCII UUID (36), public key (32), and libsodium
  Ed25519 secret key (64, seed plus public key). This entire file is secret.
- `device_id.lock`: persistent advisory lock file; the kernel releases the lock
  when its process exits. All three files use mode `0600`.

A legacy UUID-only file receives its first keypair without changing the UUID
value. The key-required marker is committed before key generation. Once marked,
missing keys never trigger regeneration. Loads check exact record size, UUID
binding, and both key components against a keypair derived from the stored seed.
Unreadable, malformed, symlinked, multiply linked, foreign-owned, or incorrectly
permissioned identity/key files cause an explicit exception. The identity
directory must be owned by the current user and must not itself be a symlink.

Writes retain the existing temporary-file/rename approach, adding exclusive
creation, file and directory fsync, and serialization with `flock`. A leftover
`.tmp` file or an incomplete marked identity fails closed, including on repeated
attempts. Recovery requires restoring the complete matching identity from a
trusted backup, not deleting keys and restarting. Back up both identity files
together. Do not run older UUID-only binaries against the migrated format or
concurrently with this version. Complete storage deletion or rollback to a
UUID-only backup cannot be distinguished from first initialization.

`get_my_device_id()` remains compatible. `get_my_device_identity()` returns the
same process-wide immutable identity; its `device_id()`, `public_key()`, and
`private_key()` accessors return const references. Identity objects cannot be
copied; references must not outlive their owning object. Temporary secret buffers
and the object's private key are cleared on destruction. Key bytes are not
logged, transmitted, or exposed by the control API.

This milestone only provides local key storage: on-disk secrets are protected by
filesystem permissions, not encryption or an OS keystore, and memory is not
locked against swapping or crash dumps. Root and processes running as the same
user remain inside the trust boundary. The filesystem adapter still uses the
existing Linux/POSIX approach; other platforms will need equivalent persistence.
Network identity is still the UUID alone, not authentication proof. No protocol,
pairing, authentication, or encryption changes are included.

`identity_tests` uses temporary directories and fresh child processes to cover
persistence, migration, permissions, corruption, interrupted writes, and
concurrent initialization without accessing the user's actual identity.

---

# Device Pairing
Gasoline supports multiple devices within the same environment.
A device must be paired before it is allowed to exchange data.
Pairing ensures that only trusted devices can communicate.
The pairing process typically involves:
1. Device discovery
2. Pair request
3. User confirmation
4. Trust establishment
Trusted devices are stored locally and reused for future sessions.

---

# Multi-Device Model
Gasoline is designed to support multiple devices simultaneously.
A single device may be connected to several others at the same time.
Examples:
* One phone connected to two laptops
* Multiple phones connected to a single desktop
* Mixed Linux and Windows environments
The Gasoline daemon maintains a device registry and manages connections accordingly.

---

# File Transfer System
Gasoline supports file sharing between connected devices.
File transfer follows a permission-based workflow:
1. Device sends a file transfer offer
2. Receiving device shows a notification
3. User accepts or rejects the transfer
4. If accepted, the file transfer begins
This prevents unwanted or malicious file transfers.

---

# Notification Mirroring
Gasoline mirrors Android notifications to desktop devices.
The Android client sends notification events to connected devices.
The platform adapter then displays the notification using native OS notification systems.
Notification replies may be supported if the originating Android notification allows it.

---

# Design Principles
Gasoline is built around several core design principles.

### Platform Agnostic Core
The core logic must not rely on operating system specific APIs.
This allows the same networking and protocol logic to run on both Linux and Windows.

### Modular Architecture
Features should be implemented as modular components.
This allows the project to expand without tightly coupling features together.

### Local Network First
Gasoline prioritizes direct device-to-device communication within the local network.
No external servers are required.

### Security Through Pairing
Devices must explicitly trust each other before communication occurs.
The persistent device ID is part of identity handling, but it is not an authentication mechanism by itself.

### Low Resource Usage
Gasoline is intended to run continuously in the background.
The system should minimize memory usage and CPU overhead.

---

# Future Extensions
Planned future capabilities may include:
* Clipboard synchronization
* Call answering support
* Device battery status
* Media control
* Remote input
These features will build on top of the existing protocol and core architecture.

---

# Repository Structure
The Gasoline repository is organized as follows:

```
gasoline/
│
├── core/            # platform independent logic
├── daemon/          # main service executable
├── platform/
│   ├── linux/       # Linux integrations
│   └── windows/     # Windows integrations
│
├── android/         # Android client
│
├── protocol_docs/   # protocol specifications
│
└── docs/            # architecture and design documentation
```

---

## Phases:
- Milestone 1: daemon + TCP server
- Milestone 2: device discovery
- Milestone 3: pairing
- Milestone 4: notifications
- Milestone 5: file transfer
