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

A session has 10 seconds from creation to complete the future authenticated
handshake and reach READY, measured with `std::chrono::steady_clock`. Partial
traffic does not reset this deadline. The legacy v2 `hello` is retained only as
temporary, unverified setup metadata: it cannot register a device, reserve UUID
ownership, authorize sends, or reach READY. Ping, pong, messages, and all other
application/control records are rejected before authentication. Until the
cryptographic handshake is implemented, production sessions intentionally cannot
reach READY.

Protocol v2 is an incompatible binary-framed transport. Each direction starts
with the ten-byte preface `GASOLINE 00 02` (eight ASCII magic bytes followed by
the network-order version 2). Both sides send and validate this preface before
either sends a framed record. Records use a two-byte unsigned
network-order length followed by exactly that many payload bytes. Record payloads
are limited to 65,535 bytes; setup/control JSON records (`hello`, `ping`, and
`pong`) are additionally limited to 4,096 bytes. The standalone v2 parser owns
preface and record reassembly, while `Packet` owns JSON parsing and schema checks.
It supports fragmented and coalesced records and rejects malformed/truncated
streams. A v1 newline-delimited JSON peer fails the preface check; there is no
protocol downgrade or plaintext fallback.

Duplicate arbitration, the READY registry transition, and ownership publication
occur in one registry operation under one lock, only after an exact ACTIVE
UUID/public-key authorization snapshot exists. A candidate cannot displace an
incumbent while it is merely being validated. The connection initiated by the
lower authenticated UUID is preferred when opposite directions compete; a live
incumbent wins same-direction ties. Unverified hello and discovery UUIDs never
enter this arbitration. Replaced sessions are returned only after the new owner
is committed, then stopped through their Connection handles; their later cleanup
cannot remove the new owner's entry.

The authorization boundary uses two capability types. `VerifiedPeerIdentity`
represents cryptographic proof of a UUID/Ed25519-public-key binding; production
currently has no constructor or factory for it because the authenticator has not
been implemented. Only the session-test target can manufacture one. An exact
ACTIVE TrustStore lookup consumes this proof-bearing identity and produces an
`AuthorizedPeer` snapshot. Registry publication and application routing consume
that immutable snapshot rather than accepting caller-supplied UUID strings.

`SessionAuthentication` also owns live authorization invalidation. Final READY
publication registers the exact UUID, Ed25519 public key, trust revision, and
permission snapshot; activation revalidates that snapshot so a revocation that
wins the authorization/activation race fails closed. A background monitor uses
the TrustStore change token to notice both writes through the daemon's store and
SQLite commits made by another connection or process, then re-runs the exact
ACTIVE lookup for each live binding. A changed revision, changed permissions,
missing binding, revoked status, or persistence error stops the matching session.
Application records are additionally revalidated immediately before send or
dispatch, so a privileged operation cannot rely only on the cached snapshot
during the monitor interval. Setup/control records do not perform this database
lookup. Cleanup unregisters by session ID, so delayed invalidation of an old
connection cannot remove a newer DeviceRegistry owner.

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
same process-wide immutable identity and exposes only `device_id()` and
`public_key()`. Identity objects cannot be copied; references must not outlive
their owning object. There is no production raw-key accessor or generic signing
oracle. The future authenticator must add a private structured,
domain-separated signing operation after its transcript format is defined.
Temporary secret buffers and the object's private key are cleared on destruction.
Key bytes are not logged, transmitted, or exposed by the control API.

This milestone only provides local key storage: on-disk secrets are protected by
filesystem permissions, not encryption or an OS keystore, and memory is not
locked against swapping or crash dumps. Root and processes running as the same
user remain inside the trust boundary. The filesystem adapter still uses the
existing Linux/POSIX approach; other platforms will need equivalent persistence.
Discovery and hello UUIDs remain unverified metadata, not authentication proof.
Production sessions deliberately cannot reach READY until the cryptographic
authenticator exists. No pairing, authentication protocol, or encryption changes
are included.

`identity_tests` uses temporary directories and fresh child processes to cover
persistence, migration, permissions, corruption, interrupted writes, and
concurrent initialization without accessing the user's actual identity.

---

## Persistent Trust Storage
`core/trust/TrustStore` is a standalone storage/model foundation, not part of
`DeviceRegistry`. The daemon owns it through `SessionAuthentication`, which is
shared by live sessions:

- `DiscoveredDevice` holds temporary discovery metadata, including an address.
- `Connection` owns a live session; `DeviceRegistry` tracks only sessions that
  crossed the proof-bearing authorization boundary. Until the authenticator is
  implemented, production sessions cannot become READY.
- `TrustedDevice` is a persistent local UUID/public-key trust decision. Neither
  discovery nor connection registration can create or reactivate this record.

The Linux database is `$XDG_STATE_HOME/gasoline/trust.sqlite3` when that variable
is an absolute path, otherwise `~/.local/state/gasoline/trust.sqlite3` (home is
resolved through the user database, as with identity). This is separate from
`~/.config/gasoline/device_id.keys`; local private keys never enter SQLite.
An explicit database path supports isolated tests and must name a file in a
dedicated application directory, since that directory is secured to `0700`.

The main database is exclusively created with mode `0600`; SQLite's Linux/Unix
VFS creates WAL, shared-memory, and journal files with matching permissions.
Existing database/sidecar files must be regular, single-link, user-owned `0600`
files, not symlinks. Checks run on opening and before data operations. The
initialization lock is also `0600`. Ancestor directories are fsynced before
database initialization, and the containing directory after initialization.
No process-wide umask changes are made. SQLite temporary storage is in memory.

### Schema
UUIDs and attempt IDs are 16-byte BLOBs; public keys are 32-byte BLOBs. The C++ API
uses fixed-size arrays with checked text-UUID/binary-public-key conversion.
SQLite STRICT tables and length checks enforce the corresponding stored types
and sizes. Key lengths are validated here, not possession or peer authenticity.

- `trusted_devices`: UUID primary key; globally unique public key, including
  revoked records; ACTIVE/REVOKED status; positive trust revision; local alias;
  untrusted peer-reported name/platform; pairing method; paired, revoked, and
  last-authenticated timestamps (UTC Unix seconds). Timestamp/status consistency
  and text lengths have CHECK constraints.
- `device_permissions`: `(device_uuid, permission)` primary key, with a foreign
  key and cascading deletion. Permission names are bounded opaque capability
  names, not an implemented feature/authorization policy.
- `pairing_attempts`: attempt UUID primary key; peer UUID/public key; revision;
  PENDING/CONFIRMED/CANCELLED/EXPIRED status; separate local/peer confirmation
  flags; creation/update/expiry timestamps. CONFIRMED requires both flags.
- `pairing_attempt_permissions`: proposed permissions keyed by
  `(attempt_id, permission)`, with a cascading attempt foreign key.

Attempts intentionally do not require an existing trusted-device record. Multiple
attempts may concern one peer. They contain no live session handles, transport
addresses, private keys, traffic keys, or confirmation secrets. Confirmation
flags are stored state only, never evidence that authentication succeeded.

### Transactions And API
Construction opens/initializes and migrates the database. Application ID `GSTR`
and `PRAGMA user_version` identify the schema. Migration 1 creates trusted devices
and permissions; migration 2 adds attempt tables. All required schema changes,
application ID, and version updates commit together in one transaction. A private
advisory lock serializes initialization across instances/processes. Existing
unversioned/unrecognized databases and newer schemas are rejected, not adopted.

Each connection uses `foreign_keys=ON`, WAL, `synchronous=FULL`, FULLMUTEX, a
five-second busy timeout, defensive mode, and `trusted_schema=OFF`. The build and
runtime require thread-safe SQLite >= 3.37.0, which is the baseline required for
STRICT table support and modern security pragmas. While SQLite 3.51.3 (and backports
3.44.6 and 3.50.7) fixed the [WAL-reset corruption bug](https://www.sqlite.org/wal.html#the_wal_reset_bug)
under concurrent manual checkpointing, TrustStore does not perform manual checkpoints,
serializes writes, and does not require 3.51.3-specific SQL features; environments
subject to concurrent external WAL checkpointing should deploy patched SQLite builds.
A per-instance mutex serializes operations; SQLite serializes writes across
instances/processes. Callers must keep the TrustStore alive until all users finish
and open a new connection after fork rather than use an inherited connection.

The small API provides the explicitly administrative `import_device_for_admin`,
record lookups, trust updates/revocation, state inspection, attempt CRUD, atomic
pairing finalization, a read-only configuration snapshot, and a non-authoritative
change token for live-session monitoring. The token combines an in-process write
generation with SQLite `data_version`; it is only an invalidation hint, and every
trust decision still uses `find_active_authorization`. Raw SQLite handles never
leave the implementation. Pairing code must not use the administrative import
path.
All writes use `BEGIN IMMEDIATE` transactions, including permission replacement.
Device/attempt snapshot reads use read transactions. Updates require an expected
revision and advance it by exactly one; stale revisions fail without partial
updates. Device UUID, public key, and status are immutable through ordinary
`update_device` calls. Revoke increments the revision and clears permissions
atomically. A revoked record can return to ACTIVE only through a new confirmed,
non-expired `finalize_pairing` operation with the same UUID/public-key binding.

`state` distinguishes UNKNOWN, PAIRING_PENDING, ACTIVE, and REVOKED, with a stored
trust decision taking precedence over attempts. PENDING and CONFIRMED attempts
without a trust record remain PAIRING_PENDING and never grant permissions.
`permissions` returns only effective permissions for ACTIVE records; all other
states return none. Future authentication must compare the stored public key and
trust status, not authorize by UUID or permissions alone.

`find_active_authorization` is the authorization-facing lookup. It atomically
matches UUID, Ed25519 public key, and ACTIVE status, then returns the matching
trust revision and effective permissions from the same read transaction. It is
used only after the authentication layer proves possession of that public key.
The generic device/key lookups are administrative/audit operations whose result
may be revoked and must not be treated as authorization.

`finalize_pairing` is the only pairing completion path. In one immediate write
transaction it checks the attempt ID and expected revision, exact peer UUID/key,
CONFIRMED status, both confirmations, and expiration; copies the attempt's
proposed permissions into a new ACTIVE revision-one record or advances and
reactivates the matching revoked record; and deletes the attempt. A stale
cancellation, duplicate/replay, expired attempt, active-record collision, or
identity mismatch fails without creating or reactivating trust.

Open, integrity-check, schema, permission, busy, and constraint errors are explicit
exceptions. SQLite errors retain their result codes. Transactions roll back on
failure; migrations never drop/recreate established trust state. Empty/truncated
existing databases, missing databases with recovery sidecars, or interrupted
initialization are rejected for manual recovery. Normal committed-WAL recovery
is left to SQLite. Backups must use SQLite's backup facilities or copy a quiesced
database together with any WAL; never discard WAL to make an open succeed.

This is plaintext, permission-protected local storage, not protection against
root, same-user tampering, backup rollback, or complete deletion of all database
state. Use a local filesystem with working SQLite locks/fsync, not a network
filesystem. There is no automatic attempt expiry/cleanup, pairing protocol,
cryptographic authentication, or encrypted transport yet. The connection layer
nevertheless fails closed: only a proof-bearing identity can request the exact
ACTIVE authorization lookup, and production cannot create that proof today. The
stored attempt expiry time is metadata until later policy explicitly
updates/deletes attempts.

`trust_store_tests` exercises schema upgrades/rollback, constraints, state and
permission isolation, revisions, concurrency, private files, and WAL recovery
entirely in temporary directories. It never loads the real local identity or
starts discovery, connections, or pairing.

---

# Device Pairing (Planned)
Gasoline supports multiple devices within the same environment. Pairing and
authenticated access are planned; the storage foundation above does not enforce
them on current sessions. The future pairing process will involve:
1. Device discovery
2. Pair request
3. User confirmation
4. Trust establishment
Trusted devices will be stored locally and checked during future authenticated sessions.

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
