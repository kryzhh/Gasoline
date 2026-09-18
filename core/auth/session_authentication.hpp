#pragma once

#include "../trust/trust_store.hpp"

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <thread>

namespace gasoline {

class SessionAuthentication;
class Connection;

// Evidence that the peer proved possession of the private key corresponding to
// this UUID/public-key binding. Production code deliberately has no way to
// construct this type until the cryptographic authenticator is implemented.
class VerifiedPeerIdentity final {
public:
    const TrustUuid& uuid() const noexcept { return uuid_; }
    const TrustPublicKey& public_key() const noexcept { return public_key_; }

#ifdef GASOLINE_SESSION_TESTING
    // The test target is compiled separately from production and is the only
    // build allowed to manufacture proof-bearing identities without crypto.
    static VerifiedPeerIdentity for_test(TrustUuid uuid, TrustPublicKey public_key) {
        return VerifiedPeerIdentity(std::move(uuid), std::move(public_key));
    }
#endif

private:
    // The future cryptographic authenticator must be explicitly granted access
    // here when it is implemented. Do not add a production UUID/key factory.
    VerifiedPeerIdentity(TrustUuid uuid, TrustPublicKey public_key)
        : uuid_(std::move(uuid)), public_key_(std::move(public_key)) {}

    TrustUuid uuid_{};
    TrustPublicKey public_key_{};
};

// An authorization snapshot for a peer that has already proved possession of
// its Ed25519 key. Only SessionAuthentication can construct this type.
class AuthorizedPeer final {
public:
    const TrustUuid& uuid() const noexcept { return uuid_; }
    const TrustPublicKey& public_key() const noexcept { return public_key_; }
    const std::string& device_id() const noexcept { return device_id_; }
    int64_t revision() const noexcept { return revision_; }
    const std::vector<std::string>& permissions() const noexcept { return permissions_; }

private:
    friend class SessionAuthentication;

    AuthorizedPeer(TrustUuid uuid, TrustPublicKey public_key, std::string device_id,
                   int64_t revision, std::vector<std::string> permissions);

    TrustUuid uuid_{};
    TrustPublicKey public_key_{};
    std::string device_id_;
    int64_t revision_ = 0;
    std::vector<std::string> permissions_;
};

// Owns the persistent trust dependency used by the future handshake. A raw
// UUID/public-key pair cannot reach the authorization lookup; callers must
// present the capability minted by the cryptographic authenticator.
class SessionAuthentication {
public:
    explicit SessionAuthentication(std::shared_ptr<TrustStore> trust_store);
    ~SessionAuthentication();

    SessionAuthentication(const SessionAuthentication&) = delete;
    SessionAuthentication& operator=(const SessionAuthentication&) = delete;

    std::optional<AuthorizedPeer> authorize_verified_peer(
        const VerifiedPeerIdentity& identity) const;

private:
    friend class Connection;

    struct LiveAuthorization {
        TrustUuid uuid{};
        TrustPublicKey public_key{};
        int64_t revision = 0;
        std::vector<std::string> permissions;
        std::function<void()> invalidate;
    };

    bool authorization_is_current(const AuthorizedPeer& authorization) const;
    bool register_live_session(uint64_t session_id,
                               const AuthorizedPeer& authorization,
                               std::function<void()> invalidate);
    void unregister_live_session(uint64_t session_id) noexcept;
    void monitor_live_authorizations();

    std::shared_ptr<TrustStore> trust_store_;
    std::mutex live_mutex_;
    std::condition_variable live_changed_;
    std::map<uint64_t, LiveAuthorization> live_authorizations_;
    bool stopping_ = false;
    bool force_scan_ = false;
    std::thread monitor_;
};

} // namespace gasoline
