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
When a device connects, both sides exchange protocol messages using JSON packets.

---

# Packet Format
All Gasoline messages are represented as JSON objects.
Each packet follows the same top-level structure.
Example packet:
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

## Packet Fields

### type
Defines the type of packet being transmitted.
Currently implemented: "hello"

### device_id
A unique identifier assigned to each device.
Example: "phone_01", "laptop_linux"

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

---

# Connection Lifecycle
When two devices connect, they follow this sequence:
```
Connection established
        ↓
hello packet exchange
        ↓
device registration
        ↓
connection maintained
```
Currently, only the hello packet is processed, registering the device in the system.

---

# Core Packet Types

## hello
Sent immediately after a connection is established.
Purpose:
* identify device
* register device in the system
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

## ping
Keepalive message.
```json
{
 "type": "ping",
 "device_id": "phone_01",
 "payload": {}
}
```

## pong
Keepalive response.
```json
{
 "type": "pong",
 "device_id": "laptop_linux",
 "payload": {}
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

---

# Protocol Versioning (Planned)
Future protocol changes may introduce versioning.
Example:
```json
{
 "protocol_version": 1
}
```
This allows backward compatibility as the protocol evolves.

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