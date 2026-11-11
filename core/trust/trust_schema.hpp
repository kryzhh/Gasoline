#pragma once

namespace gasoline::trust_schema {
inline constexpr int APPLICATION_ID = 0x47535452; // GSTR
inline constexpr int VERSION = 2;

inline constexpr const char* V1 = R"sql(
CREATE TABLE trusted_devices (
    uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid) = 16),
    public_key BLOB NOT NULL UNIQUE CHECK(length(public_key) = 32),
    status TEXT NOT NULL CHECK(status IN ('ACTIVE','REVOKED')),
    revision INTEGER NOT NULL CHECK(revision >= 1),
    local_alias TEXT NOT NULL CHECK(length(local_alias) <= 256),
    peer_name TEXT NOT NULL CHECK(length(peer_name) <= 256),
    peer_platform TEXT NOT NULL CHECK(length(peer_platform) <= 64),
    pairing_method TEXT NOT NULL CHECK(length(pairing_method) BETWEEN 1 AND 64),
    paired_at INTEGER NOT NULL CHECK(paired_at >= 0),
    revoked_at INTEGER,
    last_authenticated_at INTEGER,
    CHECK((status = 'ACTIVE' AND revoked_at IS NULL) OR
          (status = 'REVOKED' AND revoked_at IS NOT NULL AND revoked_at >= paired_at)),
    CHECK(last_authenticated_at IS NULL OR last_authenticated_at >= paired_at)
) STRICT;
CREATE TABLE device_permissions (
    device_uuid BLOB NOT NULL CHECK(length(device_uuid) = 16)
        REFERENCES trusted_devices(uuid) ON DELETE CASCADE,
    permission TEXT NOT NULL CHECK(length(permission) BETWEEN 1 AND 64),
    PRIMARY KEY(device_uuid, permission)
) STRICT;
)sql";

inline constexpr const char* V2 = R"sql(
CREATE TABLE pairing_attempts (
    attempt_id BLOB PRIMARY KEY NOT NULL CHECK(length(attempt_id) = 16),
    peer_uuid BLOB NOT NULL CHECK(length(peer_uuid) = 16),
    peer_public_key BLOB NOT NULL CHECK(length(peer_public_key) = 32),
    status TEXT NOT NULL CHECK(status IN ('PENDING','CONFIRMED','CANCELLED','EXPIRED')),
    revision INTEGER NOT NULL CHECK(revision >= 1),
    local_confirmed INTEGER NOT NULL CHECK(local_confirmed IN (0,1)),
    peer_confirmed INTEGER NOT NULL CHECK(peer_confirmed IN (0,1)),
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    updated_at INTEGER NOT NULL CHECK(updated_at >= created_at),
    expires_at INTEGER NOT NULL CHECK(expires_at >= updated_at),
    CHECK(status != 'CONFIRMED' OR (local_confirmed = 1 AND peer_confirmed = 1))
) STRICT;
CREATE INDEX pairing_attempt_peer ON pairing_attempts(peer_uuid, status);
CREATE TABLE pairing_attempt_permissions (
    attempt_id BLOB NOT NULL CHECK(length(attempt_id) = 16)
        REFERENCES pairing_attempts(attempt_id) ON DELETE CASCADE,
    permission TEXT NOT NULL CHECK(length(permission) BETWEEN 1 AND 64),
    PRIMARY KEY(attempt_id, permission)
) STRICT;
)sql";
} // namespace gasoline::trust_schema
