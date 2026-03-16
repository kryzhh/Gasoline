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
  "device_id": "phone_01",
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