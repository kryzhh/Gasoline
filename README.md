# Gasoline
Fuel your productivity without biting into the forbidden fruit

Gasoline is a cross-device connectivity system designed to allow
Linux, Windows, and Android devices to communicate over a local network.

## Current Features
- Device discovery and registration (mDNS)
- Persistent TCP connections
- Basic packet routing (hello, ping/pong)
- Multi-device support with thread-safe registry
- Packet monitoring and logging

## Planned Features
- notification mirroring
- notification replies
- file transfer
- device pairing and authentication
- Android client
- Windows support

Project Status: Active development (Linux daemon functional)

## Documentation:
- Architecture: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Protocols: [protocol_docs/PROTOCOL.md](protocol_docs/PROTOCOL.md)

## Building
```bash
mkdir build
cd build
cmake ..
make
```

_"Just pour up the gasolineeee. It don't mean much to mee"_
