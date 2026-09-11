#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gasoline {

using TrustUuid = std::array<unsigned char, 16>;
using TrustPublicKey = std::array<unsigned char, 32>;
TrustUuid trust_uuid_from_string(std::string_view value);
std::string trust_uuid_to_string(const TrustUuid& value);
TrustPublicKey trust_public_key_from_bytes(std::string_view value);

enum class TrustStatus { Active, Revoked };
enum class TrustState { Unknown, PairingPending, Active, Revoked };
enum class PairingAttemptStatus { Pending, Confirmed, Cancelled, Expired };

// Timestamps are UTC Unix seconds. These are local records, not wire packets.
struct TrustedDevice {
    TrustUuid uuid{};
    TrustPublicKey public_key{};
    TrustStatus status = TrustStatus::Active;
    int64_t revision = 1;
    std::string local_alias;
    std::string peer_name;
    std::string peer_platform;
    std::string pairing_method;
    int64_t paired_at = 0;
    std::optional<int64_t> revoked_at;
    std::optional<int64_t> last_authenticated_at;
    std::vector<std::string> permissions;
};

struct PairingAttempt {
    TrustUuid attempt_id{};
    TrustUuid peer_uuid{};
    TrustPublicKey peer_public_key{};
    PairingAttemptStatus status = PairingAttemptStatus::Pending;
    int64_t revision = 1;
    bool local_confirmed = false;
    bool peer_confirmed = false;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t expires_at = 0;
    std::vector<std::string> proposed_permissions;
};

class TrustStoreError : public std::runtime_error {
public:
    explicit TrustStoreError(const std::string& message, int sqlite_code = 0);
    int sqlite_code() const noexcept { return code_; }
private:
    int code_;
};

class TrustStore {
public:
    struct Configuration {
        int schema_version;
        bool foreign_keys;
        std::string journal_mode;
        int synchronous;
    };

    static std::filesystem::path default_database_path();
    explicit TrustStore(const std::filesystem::path& path = default_database_path());
    ~TrustStore();
    TrustStore(const TrustStore&) = delete;
    TrustStore& operator=(const TrustStore&) = delete;

    void insert_device(const TrustedDevice& device);
    std::optional<TrustedDevice> find_device(const TrustUuid& uuid) const;
    std::optional<TrustedDevice> find_by_public_key(const TrustPublicKey& key) const;
    // UUID/key are immutable. Replacement revision must be expected_revision + 1.
    // Reactivation requires this explicit local update; no discovery hook exists.
    void update_device(const TrustedDevice& device, int64_t expected_revision);
    void revoke_device(const TrustUuid& uuid, int64_t expected_revision, int64_t revoked_at);
    TrustState state(const TrustUuid& uuid) const;
    // Effective permissions only: unknown, pending, and revoked devices return none.
    std::vector<std::string> permissions(const TrustUuid& uuid) const;

    void create_attempt(const PairingAttempt& attempt);
    std::optional<PairingAttempt> find_attempt(const TrustUuid& attempt_id) const;
    void update_attempt(const PairingAttempt& attempt, int64_t expected_revision);
    void delete_attempt(const TrustUuid& attempt_id, int64_t expected_revision);
    Configuration configuration() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gasoline
