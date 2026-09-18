# Gasoline Protocol Specification

## Overview
The Gasoline protocol defines how devices communicate within the Gasoline ecosystem.
The protocol enables Android phones, Linux desktops, and Windows desktops to exchange information such as:
* notifications
* notification replies
* file transfer requests
* device discovery
* pairing information
All communication occurs over the local network using persistent connections between devices.
The protocol is designed to be:
* simple
* extensible
* platform independent
* human-readable during development

---

# Transport Layer
Gasoline uses persistent TCP connections between devices.
Each device runs a Gasoline daemon that listens on a predefined port.
Port: 42666
When a device connects, both sides speak the explicitly incompatible protocol v2
wire format. There is no protocol v1 or newline-delimited plaintext fallback.

## Protocol v2 Preface

Each direction begins with exactly ten bytes before its first record:

```
47 41 53 4f 4c 49 4e 45 00 02
 G  A  S  O  L  I  N  E   v2
```

The first eight bytes are the ASCII magic `GASOLINE`; the final two bytes are the
unsigned network-order protocol version, currently 2. A missing, malformed, or
unsupported preface terminates the session. The preface occurs once per direction.
Both endpoints send the preface first and validate the peer preface before either
endpoint sends its first framed record.

## Protocol v2 Records

After the preface, every record consists of a two-byte unsigned network-order
payload length followed by exactly that many payload bytes. Lengths from 0 through
65,535 are valid at the framing layer. Empty records are therefore representable,
although the current JSON protocol rejects them as invalid application payloads.
`hello`, `ping`, and `pong` JSON records have an additional 4,096-byte limit.

The framing parser validates the complete length before reserving payload storage,
handles arbitrary stream fragmentation and multiple records per read, and treats
EOF in a preface, header, or payload as truncation. Framing produces opaque record
payload bytes; JSON parsing is a separate layer above it so future encrypted record
payloads do not require a new transport parser.

---

# Packet Format
All Gasoline messages are represented as JSON objects.
Each packet follows the same top-level structure.
Example packet:
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

## Packet Fields

### type
Defines the type of packet being transmitted.
Currently implemented: "hello", "ping", "pong"

### device_id
A persistent device identifier assigned to each Gasoline installation.
It is a UUID-like 128-bit random value stored locally and reused across daemon restarts.
Example: "550e8400-e29b-41d4-a716-446655440000"

### payload
Packet-specific data associated with the message type.
Optional field.

---

# Device Identification
Each Gasoline device has a unique device identifier.
Device information includes:
- device_id: Unique string identifier
- device_name: Human-readable name
- device_type: Platform type ("android", "linux", "windows")

The device identifier is an identity mechanism, not proof of authentication.
Possession of a device_id alone must not be treated as trust or authorization.

---

# Connection Lifecycle
The current authentication-preparation milestone follows this sequence:
```
Device discovery (mDNS)
        ↓
Connection established
        ↓
protocol-v2 preface and temporary hello metadata
        ↓
wait for the future authenticated handshake
```
Discovery and hello UUIDs are untrusted routing/setup hints. Hello no longer
registers a device or grants UUID ownership. Production sessions cannot reach
READY until the authenticated handshake is implemented; application packets and
ping/pong are rejected before READY.

---

# Core Packet Types

## hello
Sent immediately after a connection is established as temporary compatibility
metadata.
Purpose:
* carry legacy display metadata while the authenticated replacement is pending

It is not authenticated, does not register a device, and cannot authorize traffic.
Example:
```json
{
 "type": "hello",
 "device_id": "phone_01",
 "payload": {
   "device_name": "Krish Phone",
   "device_type": "android"
 }
}
```

## ping
Keepalive message sent to check connection status.
```json
{
 "type": "ping",
 "device_id": "phone_01",
 "payload": {}
}
```

## pong
Response to ping, confirms connection is active.
```json
{
 "type": "pong",
 "device_id": "laptop_linux",
 "payload": {}
}
```

---

# Planned Packet Types

## pair_request
Sent by a device attempting to pair with another device.
```json
{
 "type": "pair_request",
 "device_id": "phone_01",
 "payload": {
   "device_name": "Krish Phone"
 }
}
```

## pair_accept
Sent when the user accepts the pairing request.
```json
{
 "type": "pair_accept",
 "device_id": "laptop_linux",
 "payload": {}
}
```

## notification
Used by the Android client to mirror notifications.
```json
{
 "type": "notification",
 "device_id": "phone_01",
 "payload": {
   "app": "WhatsApp",
   "title": "John",
   "message": "Hello"
 }
}
```

## notification_reply
Used by desktop devices to send a reply to a notification.
```json
{
 "type": "notification_reply",
 "device_id": "laptop_linux",
 "payload": {
   "notification_id": "12345",
   "reply_text": "On my way"
 }
}
```

## file_offer
Sent when a device wants to send a file.
```json
{
 "type": "file_offer",
 "device_id": "phone_01",
 "payload": {
   "file_name": "photo.jpg",
   "file_size": 204800
 }
}
```

## file_accept
Sent when the receiving device accepts the file transfer.
```json
{
 "type": "file_accept",
 "device_id": "laptop_linux",
 "payload": {
   "file_name": "photo.jpg"
 }
}
```

## file_reject
Sent when the receiving device rejects the transfer.
```json
{
 "type": "file_reject",
 "device_id": "laptop_linux",
 "payload": {
   "file_name": "photo.jpg"
 }
}
```

---

# Security Model (Planned)
Gasoline will use a trust-based pairing model.
Unpaired devices cannot exchange normal packets.
During pairing:
* user confirmation is required
* the device is added to a trusted device list
Trusted devices are stored locally.

The persistent device ID introduced in the core does not replace pairing, trust management, or authentication.

---

# Protocol Versioning

The binary preface carries the wire version. Version 2 endpoints accept only the
exact version-2 preface and never probe for or downgrade to the former
newline-delimited JSON format.
Wire-version changes are intentionally incompatible; compatibility requires an
explicitly separate endpoint or protocol implementation.

---

# Future Extensions
The protocol is designed to allow additional message types.
Possible future extensions include:
```
clipboard_sync
battery_status
call_event
media_control
```
New packet types can be added without breaking existing functionality.

---
