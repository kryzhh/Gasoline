#include "session_authentication.hpp"

#include <stdexcept>
#include <chrono>
#include <utility>

namespace gasoline {

AuthorizedPeer::AuthorizedPeer(TrustUuid uuid, TrustPublicKey public_key,
                               std::string device_id, int64_t revision,
                               std::vector<std::string> permissions)
    : uuid_(uuid), public_key_(public_key), device_id_(std::move(device_id)),
      revision_(revision), permissions_(std::move(permissions)) {}

SessionAuthentication::SessionAuthentication(std::shared_ptr<TrustStore> trust_store)
    : trust_store_(std::move(trust_store)) {
    if (!trust_store_) {
        throw std::invalid_argument("SessionAuthentication requires a TrustStore");
    }
    monitor_ = std::thread(&SessionAuthentication::monitor_live_authorizations, this);
}

SessionAuthentication::~SessionAuthentication() {
    {
        std::lock_guard<std::mutex> lock(live_mutex_);
        stopping_ = true;
    }
    live_changed_.notify_all();
    if (monitor_.joinable()) {
        monitor_.join();
    }
}

std::optional<AuthorizedPeer> SessionAuthentication::authorize_verified_peer(
    const VerifiedPeerIdentity& identity) const {
    auto authorization = trust_store_->find_active_authorization(
        identity.uuid(), identity.public_key());
    if (!authorization) {
        return std::nullopt;
    }
    return AuthorizedPeer(identity.uuid(), identity.public_key(),
                          trust_uuid_to_string(identity.uuid()),
                          authorization->revision, std::move(authorization->permissions));
}

bool SessionAuthentication::authorization_is_current(
    const AuthorizedPeer& authorization) const {
    try {
        const auto current = trust_store_->find_active_authorization(
            authorization.uuid(), authorization.public_key());
        return current && current->revision == authorization.revision() &&
               current->permissions == authorization.permissions();
    } catch (...) {
        return false;
    }
}

bool SessionAuthentication::register_live_session(
    uint64_t session_id, const AuthorizedPeer& authorization,
    std::function<void()> invalidate) {
    if (!invalidate || !authorization_is_current(authorization)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(live_mutex_);
    if (stopping_ || live_authorizations_.count(session_id) != 0) {
        return false;
    }
    live_authorizations_.emplace(
        session_id,
        LiveAuthorization{authorization.uuid(), authorization.public_key(),
                          authorization.revision(), authorization.permissions(),
                          std::move(invalidate)});
    // Force an exact scan after publication. This closes the window where a
    // commit could race the validation immediately above.
    force_scan_ = true;
    live_changed_.notify_one();
    return true;
}

void SessionAuthentication::unregister_live_session(uint64_t session_id) noexcept {
    std::lock_guard<std::mutex> lock(live_mutex_);
    live_authorizations_.erase(session_id);
}

void SessionAuthentication::monitor_live_authorizations() {
    using namespace std::chrono_literals;
    std::optional<TrustStore::ChangeToken> last_token;

    for (;;) {
        bool force_scan = false;
        {
            std::unique_lock<std::mutex> lock(live_mutex_);
            if (live_authorizations_.empty() && !force_scan_) {
                live_changed_.wait(lock, [this] {
                    return stopping_ || force_scan_ || !live_authorizations_.empty();
                });
            } else {
                live_changed_.wait_for(lock, 100ms, [this] {
                    return stopping_ || force_scan_;
                });
            }
            if (stopping_) {
                return;
            }
            force_scan = force_scan_;
            force_scan_ = false;
        }

        bool scan = force_scan;
        try {
            const auto token = trust_store_->change_token();
            scan = scan || !last_token || token != *last_token;
            last_token = token;
        } catch (...) {
            // Persistence uncertainty is an authorization failure, not a
            // reason to retain cached trust.
            scan = true;
            last_token.reset();
        }
        if (!scan) {
            continue;
        }

        std::map<uint64_t, LiveAuthorization> snapshot;
        {
            std::lock_guard<std::mutex> lock(live_mutex_);
            snapshot = live_authorizations_;
        }

        for (const auto& entry : snapshot) {
            const auto& authorization = entry.second;
            bool current = false;
            try {
                const auto stored = trust_store_->find_active_authorization(
                    authorization.uuid, authorization.public_key);
                current = stored && stored->revision == authorization.revision &&
                          stored->permissions == authorization.permissions;
            } catch (...) {
                current = false;
            }
            if (current) {
                continue;
            }

            std::function<void()> invalidate;
            {
                std::lock_guard<std::mutex> lock(live_mutex_);
                const auto live = live_authorizations_.find(entry.first);
                if (live == live_authorizations_.end() ||
                    live->second.revision != authorization.revision ||
                    live->second.uuid != authorization.uuid ||
                    live->second.public_key != authorization.public_key) {
                    continue;
                }
                invalidate = std::move(live->second.invalidate);
                live_authorizations_.erase(live);
            }
            if (invalidate) {
                invalidate();
            }
        }
    }
}

} // namespace gasoline
