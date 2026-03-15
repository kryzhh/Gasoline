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
Example port:
```
42666
```
When a device connects, both sides exchange protocol messages using JSON packets.

---

# Packet Format
All Gasoline messages are represented as JSON objects.
Each packet follows the same top-level structure.
Example packet:
```json
{
  "type": "notification",
  "device_id": "phone_01",
  "timestamp": 1710000000,
  "payload": {}
}
```

## Packet Fields

### type
Defines the type of packet being transmitted.
Example:
```
notification
file_offer
pair_request
```

### device_id
A unique identifier assigned to each device.
Example:

```
phone_01
laptop_linux
desktop_windows
```

### timestamp
Unix timestamp indicating when the packet was generated.

### payload
Packet-specific data associated with the message type.

---

# Device Identification
Each Gasoline device has a unique device identifier.
Example device object:

```json
{
  "device_id": "phone_01",
  "device_name": "Krish Phone",
  "device_type": "android"
}
```

Possible device types include:
```
android
linux
windows
```
Devices store trusted devices locally after pairing.

---

# Connection Lifecycle
When two devices connect, they follow this sequence.
```
Connection established
        ↓
hello packet exchange
        ↓
pairing validation
        ↓
normal packet communication
```
If a device is not trusted, pairing must occur before normal communication.

---

# Core Packet Types

## hello
Sent immediately after a connection is established.
Purpose:
* identify device
* exchange device metadata
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

# Pairing Packets

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
Once pairing is accepted, both devices store each other as trusted.

---

# Notification Packets

## notification
Used by the Android client to mirror notifications.
Example:
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
Desktop systems display the notification through native notification systems.

---

# Notification Reply

## notification_reply
Used by desktop devices to send a reply to a notification.
Example:
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
The Android client executes the reply using the Android notification API.

---

# File Transfer Protocol
File transfer is permission-based.
The sending device must first request permission.

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
The receiving device presents the user with an accept or reject prompt.

---

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

---

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

# Keepalive Packets
To maintain persistent connections, devices periodically exchange keepalive messages.

## ping
```json
{
 "type": "ping",
 "device_id": "phone_01",
 "payload": {}
}
```

## pong
```json
{
 "type": "pong",
 "device_id": "laptop_linux",
 "payload": {}
}
```

---

# Security Model
Gasoline uses a trust-based pairing model.
Unpaired devices cannot exchange normal packets.
During pairing:
* user confirmation is required
* the device is added to a trusted device list
Trusted devices are stored locally.

---

# Protocol Versioning
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