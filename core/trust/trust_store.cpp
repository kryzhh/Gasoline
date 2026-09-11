#include "trust_store.hpp"
#include "trust_schema.hpp"

#include <sqlite3.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace gasoline {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw TrustStoreError(message);
}

class Fd {
public:
    explicit Fd(int value) : value_(value) { require(value >= 0, "cannot open storage handle"); }
    ~Fd() { ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

void sync_fd(int fd) {
    while (::fsync(fd) != 0) {
        if (errno != EINTR) throw TrustStoreError("cannot synchronize storage directory");
    }
}

void sync_tree(int fd) {
    auto current = std::make_unique<Fd>(::fcntl(fd, F_DUPFD_CLOEXEC, 0));
    for (;;) {
        sync_fd(current->get());
        auto parent = std::make_unique<Fd>(::openat(current->get(), "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        struct stat a{}, b{};
        require(::fstat(current->get(), &a) == 0 && ::fstat(parent->get(), &b) == 0, "cannot inspect storage ancestors");
        if (a.st_dev == b.st_dev && a.st_ino == b.st_ino) return;
        current = std::move(parent);
    }
}

bool inspect_file(int directory, const std::string& name, struct stat* output = nullptr) {
    struct stat info{};
    if (::fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return false;
        throw TrustStoreError("cannot inspect database files");
    }
    require(S_ISREG(info.st_mode) && info.st_uid == geteuid() && info.st_nlink == 1 &&
            (info.st_mode & 07777) == 0600, "database files must be single-link, user-owned regular 0600 files");
    if (output) *output = info;
    return true;
}

void check_sql(int result, sqlite3* db) {
    if (result != SQLITE_OK) throw TrustStoreError(sqlite3_errmsg(db), result);
}

struct DatabaseDeleter { void operator()(sqlite3* db) const { sqlite3_close_v2(db); } };
using Database = std::unique_ptr<sqlite3, DatabaseDeleter>;

void execute(sqlite3* db, const std::string& sql) {
    check_sql(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), db);
}

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        check_sql(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr), db);
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    template<size_t N> void bind(int index, const std::array<unsigned char, N>& value) {
        check_sql(sqlite3_bind_blob(stmt_, index, value.data(), N, SQLITE_TRANSIENT), db_);
    }
    void bind(int index, int64_t value) { check_sql(sqlite3_bind_int64(stmt_, index, value), db_); }
    void bind(int index, const std::optional<int64_t>& value) {
        if (value) bind(index, *value);
        else check_sql(sqlite3_bind_null(stmt_, index), db_);
    }
    void bind(int index, const std::string& value) {
        require(value.size() <= 4096 && value.find('\0') == std::string::npos, "invalid text field");
        check_sql(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT), db_);
    }
    bool row() {
        const int result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw TrustStoreError(sqlite3_errmsg(db_), result);
    }
    void run() { require(!row(), "unexpected SQL result"); }
    int64_t integer(int column) const { return sqlite3_column_int64(stmt_, column); }
    std::optional<int64_t> optional_integer(int column) const {
        if (sqlite3_column_type(stmt_, column) == SQLITE_NULL) return std::nullopt;
        return integer(column);
    }
    std::string text(int column) const {
        require(sqlite3_column_type(stmt_, column) == SQLITE_TEXT, "invalid stored text");
        const auto* value = sqlite3_column_text(stmt_, column);
        require(value != nullptr, "cannot read stored text");
        return {reinterpret_cast<const char*>(value), static_cast<size_t>(sqlite3_column_bytes(stmt_, column))};
    }
    template<size_t N> std::array<unsigned char, N> blob(int column) const {
        require(sqlite3_column_type(stmt_, column) == SQLITE_BLOB && sqlite3_column_bytes(stmt_, column) == N,
                "invalid stored UUID/public-key length or type");
        std::array<unsigned char, N> result{};
        const auto* bytes = sqlite3_column_blob(stmt_, column);
        require(bytes != nullptr, "cannot read stored blob");
        std::memcpy(result.data(), bytes, N);
        return result;
    }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db, bool write = true) : db_(db) {
        execute(db, write ? "BEGIN IMMEDIATE" : "BEGIN");
    }
    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { execute(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
    bool committed_ = false;
};

int pragma_integer(sqlite3* db, const char* sql) {
    Statement query(db, sql);
    require(query.row(), "missing pragma result");
    return static_cast<int>(query.integer(0));
}

std::string pragma_text(sqlite3* db, const char* sql) {
    Statement query(db, sql);
    require(query.row(), "missing pragma result");
    return query.text(0);
}

std::string status_text(TrustStatus status) {
    switch (status) {
    case TrustStatus::Active: return "ACTIVE";
    case TrustStatus::Revoked: return "REVOKED";
    }
    throw TrustStoreError("invalid trust status");
}

std::string status_text(PairingAttemptStatus status) {
    switch (status) {
    case PairingAttemptStatus::Pending: return "PENDING";
    case PairingAttemptStatus::Confirmed: return "CONFIRMED";
    case PairingAttemptStatus::Cancelled: return "CANCELLED";
    case PairingAttemptStatus::Expired: return "EXPIRED";
    }
    throw TrustStoreError("invalid attempt status");
}

void next_revision(int64_t revision, int64_t expected) {
    require(expected > 0 && expected < std::numeric_limits<int64_t>::max() && revision == expected + 1,
            "replacement revision must advance by one");
}

void changed_one(sqlite3* db) {
    require(sqlite3_changes(db) == 1, "record missing, immutable identity changed, or stale revision");
}

void write_permissions(sqlite3* db, const TrustUuid& id, const std::vector<std::string>& permissions,
                       bool attempt) {
    require(permissions.size() <= 128, "too many permissions");
    const char* sql = attempt ? "INSERT INTO pairing_attempt_permissions VALUES(?,?)" :
                                "INSERT INTO device_permissions VALUES(?,?)";
    for (const auto& permission : permissions) {
        Statement insert(db, sql);
        insert.bind(1, id);
        insert.bind(2, permission);
        insert.run();
    }
}

std::vector<std::string> read_permissions(sqlite3* db, const TrustUuid& id, bool attempt) {
    Statement query(db, attempt ?
        "SELECT permission FROM pairing_attempt_permissions WHERE attempt_id=? ORDER BY permission" :
        "SELECT p.permission FROM device_permissions p JOIN trusted_devices d ON d.uuid=p.device_uuid "
        "WHERE d.uuid=? AND d.status='ACTIVE' ORDER BY p.permission");
    query.bind(1, id);
    std::vector<std::string> result;
    while (query.row()) result.push_back(query.text(0));
    return result;
}

constexpr const char* DEVICE_COLUMNS =
    "uuid,public_key,status,revision,local_alias,peer_name,peer_platform,pairing_method,"
    "paired_at,revoked_at,last_authenticated_at";

std::optional<TrustedDevice> read_device(sqlite3* db, Statement& query) {
    if (!query.row()) return std::nullopt;
    TrustedDevice d;
    d.uuid = query.blob<16>(0);
    d.public_key = query.blob<32>(1);
    const auto status = query.text(2);
    require(status == "ACTIVE" || status == "REVOKED", "invalid stored trust status");
    d.status = status == "ACTIVE" ? TrustStatus::Active : TrustStatus::Revoked;
    d.revision = query.integer(3);
    d.local_alias = query.text(4);
    d.peer_name = query.text(5);
    d.peer_platform = query.text(6);
    d.pairing_method = query.text(7);
    d.paired_at = query.integer(8);
    d.revoked_at = query.optional_integer(9);
    d.last_authenticated_at = query.optional_integer(10);
    d.permissions = read_permissions(db, d.uuid, false);
    return d;
}

void bind_device(Statement& query, const TrustedDevice& d) {
    require(d.status != TrustStatus::Revoked || d.permissions.empty(), "revoked devices cannot be granted permissions");
    query.bind(1, d.uuid);
    query.bind(2, d.public_key);
    query.bind(3, status_text(d.status));
    query.bind(4, d.revision);
    query.bind(5, d.local_alias);
    query.bind(6, d.peer_name);
    query.bind(7, d.peer_platform);
    query.bind(8, d.pairing_method);
    query.bind(9, d.paired_at);
    query.bind(10, d.revoked_at);
    query.bind(11, d.last_authenticated_at);
}

void bind_attempt(Statement& query, const PairingAttempt& a) {
    query.bind(1, a.attempt_id);
    query.bind(2, a.peer_uuid);
    query.bind(3, a.peer_public_key);
    query.bind(4, status_text(a.status));
    query.bind(5, a.revision);
    query.bind(6, static_cast<int64_t>(a.local_confirmed));
    query.bind(7, static_cast<int64_t>(a.peer_confirmed));
    query.bind(8, a.created_at);
    query.bind(9, a.updated_at);
    query.bind(10, a.expires_at);
}
} // namespace

TrustStoreError::TrustStoreError(const std::string& message, int code)
    : std::runtime_error("Trust store error: " + message), code_(code) {}

TrustUuid trust_uuid_from_string(std::string_view value) {
    require(value.size() == 36, "UUID must have 36 characters");
    auto hex = [](char c) -> unsigned char {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw TrustStoreError("invalid UUID hex digit");
    };
    TrustUuid result{};
    size_t input = 0;
    for (size_t i = 0; i < result.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) require(value[input++] == '-', "invalid UUID separator");
        result[i] = static_cast<unsigned char>((hex(value[input]) << 4) | hex(value[input + 1]));
        input += 2;
    }
    return result;
}

std::string trust_uuid_to_string(const TrustUuid& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += digits[value[i] >> 4];
        result += digits[value[i] & 15];
    }
    return result;
}

TrustPublicKey trust_public_key_from_bytes(std::string_view value) {
    require(value.size() == 32, "Ed25519 public key must have 32 bytes");
    TrustPublicKey result{};
    std::memcpy(result.data(), value.data(), result.size());
    return result;
}

struct TrustStore::Impl {
    std::filesystem::path path;
    std::unique_ptr<Fd> directory;
    Database db;
    mutable std::mutex mutex;

    void check_files() const {
        const auto name = path.filename().string();
        require(inspect_file(directory->get(), name), "established database is missing");
        for (const auto* suffix : {"-wal", "-shm", "-journal"}) inspect_file(directory->get(), name + suffix);
    }

    explicit Impl(const std::filesystem::path& input) {
        require(sqlite3_threadsafe() != 0 && sqlite3_libversion_number() >= 3037000,
                "a thread-safe SQLite >= 3.37.0 is required");
        path = std::filesystem::absolute(input);
        const auto name = path.filename().string();
        require(!name.empty() && name != "." && name != "..", "invalid database filename");
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) throw TrustStoreError("cannot create state directory: " + error.message());
        directory = std::make_unique<Fd>(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat info{};
        require(::fstat(directory->get(), &info) == 0 && info.st_uid == geteuid(), "state directory must be user-owned");
        require(::fchmod(directory->get(), 0700) == 0, "cannot secure state directory");
        sync_tree(directory->get());
        // Canonical spelling keeps SQLite's filename-based WAL/locking identity stable.
        path = std::filesystem::canonical(path.parent_path()) / name;
        const auto lock_name = name + ".lock";
        Fd lock(::openat(directory->get(), lock_name.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
        inspect_file(directory->get(), lock_name);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
            if ((errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) || std::chrono::steady_clock::now() >= deadline)
                throw TrustStoreError("cannot acquire database initialization lock");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const bool existing = inspect_file(directory->get(), name, &info);
        bool sidecar = false;
        for (const auto* suffix : {"-wal", "-shm", "-journal"})
            sidecar = inspect_file(directory->get(), name + suffix) || sidecar;
        require(existing || !sidecar, "database is missing but SQLite recovery files remain");
        if (existing) require(info.st_size >= 100, "database is truncated or initialization was interrupted");
        else {
            Fd file(::openat(directory->get(), name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            require(::fchmod(file.get(), 0600) == 0, "cannot secure database file");
            sync_fd(file.get());
            sync_fd(directory->get());
        }

        sqlite3* handle = nullptr;
        const int opened = sqlite3_open_v2(path.c_str(), &handle,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
        db.reset(handle);
        if (opened != SQLITE_OK) throw TrustStoreError("cannot open SQLite database", opened);
        check_sql(sqlite3_extended_result_codes(db.get(), 1), db.get());
        check_sql(sqlite3_busy_timeout(db.get(), 5000), db.get());
        check_sql(sqlite3_db_config(db.get(), SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr), db.get());
        execute(db.get(), "PRAGMA trusted_schema=OFF; PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;");
        require(pragma_integer(db.get(), "PRAGMA foreign_keys") == 1, "SQLite foreign keys unavailable");
        require(pragma_text(db.get(), "PRAGMA quick_check") == "ok", "database integrity check failed");
        const int version = pragma_integer(db.get(), "PRAGMA user_version");
        const int app = pragma_integer(db.get(), "PRAGMA application_id");
        require(existing ? (app == trust_schema::APPLICATION_ID && version >= 1 && version <= trust_schema::VERSION) :
                           (app == 0 && version == 0), "unrecognized database or unsupported schema version");
        require(pragma_text(db.get(), "PRAGMA journal_mode=WAL") == "wal", "SQLite WAL mode unavailable");
        execute(db.get(), "PRAGMA synchronous=FULL");
        require(pragma_integer(db.get(), "PRAGMA synchronous") == 2, "SQLite FULL synchronization unavailable");
        {
            Transaction migration(db.get());
            if (version < 1) execute(db.get(), trust_schema::V1);
            if (version < 2) execute(db.get(), trust_schema::V2);
            execute(db.get(), "PRAGMA application_id=" + std::to_string(trust_schema::APPLICATION_ID));
            execute(db.get(), "PRAGMA user_version=" + std::to_string(trust_schema::VERSION));
            Statement foreign_keys(db.get(), "PRAGMA foreign_key_check");
            require(!foreign_keys.row(), "database has foreign-key violations");
            // Preparing these queries also detects missing tables/columns in existing stores.
            Statement devices(db.get(), std::string("SELECT ") + DEVICE_COLUMNS + " FROM trusted_devices LIMIT 0");
            devices.run();
            Statement attempts(db.get(), "SELECT attempt_id,peer_uuid,peer_public_key,status,revision,local_confirmed,"
                "peer_confirmed,created_at,updated_at,expires_at FROM pairing_attempts LIMIT 0");
            attempts.run();
            read_permissions(db.get(), TrustUuid{}, false);
            read_permissions(db.get(), TrustUuid{}, true);
            check_files();
            migration.commit();
        }
        sync_fd(directory->get());
    }
};

std::filesystem::path TrustStore::default_database_path() {
    const char* state = std::getenv("XDG_STATE_HOME");
    if (state && std::filesystem::path(state).is_absolute()) return std::filesystem::path(state) / "gasoline" / "trust.sqlite3";
    std::vector<char> buffer(16384);
    struct passwd user{}, *result = nullptr;
    for (;;) {
        const int error = ::getpwuid_r(::getuid(), &user, buffer.data(), buffer.size(), &result);
        if (error == ERANGE && buffer.size() < 1024 * 1024) {
            buffer.resize(buffer.size() * 2);
            continue;
        }
        require(error == 0 && result && user.pw_dir && user.pw_dir[0], "cannot determine home directory");
        return std::filesystem::path(user.pw_dir) / ".local" / "state" / "gasoline" / "trust.sqlite3";
    }
}

TrustStore::TrustStore(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {}
TrustStore::~TrustStore() = default;

void TrustStore::import_device_for_admin(const TrustedDevice& device) {
    require(device.revision == 1, "initial trust revision must be one");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db);
    Statement insert(db, "INSERT INTO trusted_devices VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    bind_device(insert, device);
    insert.run();
    write_permissions(db, device.uuid, device.permissions, false);
    transaction.commit();
}

std::optional<TrustedDevice> TrustStore::find_device(const TrustUuid& uuid) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db, false);
    Statement query(db, std::string("SELECT ") + DEVICE_COLUMNS + " FROM trusted_devices WHERE uuid=?");
    query.bind(1, uuid);
    auto result = read_device(db, query);
    transaction.commit();
    return result;
}

std::optional<TrustedDevice> TrustStore::find_by_public_key(const TrustPublicKey& key) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db, false);
    Statement query(db, std::string("SELECT ") + DEVICE_COLUMNS + " FROM trusted_devices WHERE public_key=?");
    query.bind(1, key);
    auto result = read_device(db, query);
    transaction.commit();
    return result;
}

std::optional<ActivePeerAuthorization> TrustStore::find_active_authorization(
    const TrustUuid& uuid, const TrustPublicKey& public_key) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db, false);
    std::optional<ActivePeerAuthorization> result;
    {
        Statement query(db, "SELECT revision FROM trusted_devices "
                            "WHERE uuid=? AND public_key=? AND status='ACTIVE'");
        query.bind(1, uuid);
        query.bind(2, public_key);
        if (query.row()) {
            ActivePeerAuthorization authorization;
            authorization.revision = query.integer(0);
            result = std::move(authorization);
        }
    }
    if (result) result->permissions = read_permissions(db, uuid, false);
    transaction.commit();
    return result;
}

void TrustStore::update_device(const TrustedDevice& device, int64_t expected_revision) {
    next_revision(device.revision, expected_revision);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db);
    Statement update(db, "UPDATE trusted_devices SET status=?3,revision=?4,local_alias=?5,peer_name=?6,peer_platform=?7,"
        "pairing_method=?8,paired_at=?9,revoked_at=?10,last_authenticated_at=?11 "
        "WHERE uuid=?1 AND public_key=?2 AND status=?3 AND revision=?12");
    bind_device(update, device);
    update.bind(12, expected_revision);
    update.run();
    changed_one(db);
    Statement clear(db, "DELETE FROM device_permissions WHERE device_uuid=?");
    clear.bind(1, device.uuid);
    clear.run();
    write_permissions(db, device.uuid, device.permissions, false);
    transaction.commit();
}

void TrustStore::revoke_device(const TrustUuid& uuid, int64_t expected_revision, int64_t revoked_at) {
    require(expected_revision > 0 && expected_revision < std::numeric_limits<int64_t>::max(), "invalid trust revision");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db);
    Statement update(db, "UPDATE trusted_devices SET status='REVOKED',revision=revision+1,revoked_at=? "
                         "WHERE uuid=? AND revision=?");
    update.bind(1, revoked_at);
    update.bind(2, uuid);
    update.bind(3, expected_revision);
    update.run();
    changed_one(db);
    Statement clear(db, "DELETE FROM device_permissions WHERE device_uuid=?");
    clear.bind(1, uuid);
    clear.run();
    transaction.commit();
}

TrustState TrustStore::state(const TrustUuid& uuid) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    Statement query(impl_->db.get(), "SELECT COALESCE((SELECT status FROM trusted_devices WHERE uuid=?1),"
        "(SELECT 'PENDING' FROM pairing_attempts WHERE peer_uuid=?1 AND status IN ('PENDING','CONFIRMED') LIMIT 1),'UNKNOWN')");
    query.bind(1, uuid);
    require(query.row(), "missing trust state result");
    const auto value = query.text(0);
    if (value == "ACTIVE") return TrustState::Active;
    if (value == "REVOKED") return TrustState::Revoked;
    if (value == "PENDING") return TrustState::PairingPending;
    require(value == "UNKNOWN", "invalid stored trust state");
    return TrustState::Unknown;
}

std::vector<std::string> TrustStore::permissions(const TrustUuid& uuid) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    return read_permissions(impl_->db.get(), uuid, false);
}

void TrustStore::create_attempt(const PairingAttempt& attempt) {
    require(attempt.revision == 1, "initial attempt revision must be one");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db);
    Statement insert(db, "INSERT INTO pairing_attempts VALUES(?,?,?,?,?,?,?,?,?,?)");
    bind_attempt(insert, attempt);
    insert.run();
    write_permissions(db, attempt.attempt_id, attempt.proposed_permissions, true);
    transaction.commit();
}

std::optional<PairingAttempt> TrustStore::find_attempt(const TrustUuid& id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db, false);
    Statement query(db, "SELECT attempt_id,peer_uuid,peer_public_key,status,revision,local_confirmed,peer_confirmed,"
                        "created_at,updated_at,expires_at FROM pairing_attempts WHERE attempt_id=?");
    query.bind(1, id);
    std::optional<PairingAttempt> result;
    if (query.row()) {
        PairingAttempt a;
        a.attempt_id = query.blob<16>(0);
        a.peer_uuid = query.blob<16>(1);
        a.peer_public_key = query.blob<32>(2);
        const auto status = query.text(3);
        if (status == "PENDING") a.status = PairingAttemptStatus::Pending;
        else if (status == "CONFIRMED") a.status = PairingAttemptStatus::Confirmed;
        else if (status == "CANCELLED") a.status = PairingAttemptStatus::Cancelled;
        else if (status == "EXPIRED") a.status = PairingAttemptStatus::Expired;
        else throw TrustStoreError("invalid stored attempt status");
        a.revision = query.integer(4);
        a.local_confirmed = query.integer(5) != 0;
        a.peer_confirmed = query.integer(6) != 0;
        a.created_at = query.integer(7);
        a.updated_at = query.integer(8);
        a.expires_at = query.integer(9);
        a.proposed_permissions = read_permissions(db, id, true);
        result = std::move(a);
    }
    transaction.commit();
    return result;
}

void TrustStore::update_attempt(const PairingAttempt& attempt, int64_t expected_revision) {
    next_revision(attempt.revision, expected_revision);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db);
    Statement update(db, "UPDATE pairing_attempts SET status=?4,revision=?5,local_confirmed=?6,peer_confirmed=?7,"
        "updated_at=?9,expires_at=?10 WHERE attempt_id=?1 AND peer_uuid=?2 AND peer_public_key=?3 AND created_at=?8 AND revision=?11");
    bind_attempt(update, attempt);
    update.bind(11, expected_revision);
    update.run();
    changed_one(db);
    Statement clear(db, "DELETE FROM pairing_attempt_permissions WHERE attempt_id=?");
    clear.bind(1, attempt.attempt_id);
    clear.run();
    write_permissions(db, attempt.attempt_id, attempt.proposed_permissions, true);
    transaction.commit();
}

void TrustStore::delete_attempt(const TrustUuid& id, int64_t expected_revision) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db);
    Statement remove(db, "DELETE FROM pairing_attempts WHERE attempt_id=? AND revision=?");
    remove.bind(1, id);
    remove.bind(2, expected_revision);
    remove.run();
    changed_one(db);
    transaction.commit();
}

void TrustStore::finalize_pairing(const TrustUuid& attempt_id, int64_t expected_revision,
                                  const PairingFinalization& finalization) {
    require(expected_revision > 0, "invalid attempt revision");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->check_files();
    auto* db = impl_->db.get();
    Transaction transaction(db);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    require(now >= 0, "system clock precedes Unix epoch");

    {
        Statement attempt(db, "SELECT peer_uuid,peer_public_key FROM pairing_attempts "
                              "WHERE attempt_id=? AND revision=? AND status='CONFIRMED' "
                              "AND local_confirmed=1 AND peer_confirmed=1 AND expires_at>=?");
        attempt.bind(1, attempt_id);
        attempt.bind(2, expected_revision);
        attempt.bind(3, now);
        require(attempt.row(), "pairing attempt missing, stale, unconfirmed, consumed, or expired");
        require(attempt.blob<16>(0) == finalization.peer_uuid &&
                attempt.blob<32>(1) == finalization.peer_public_key,
                "pairing finalization identity does not match attempt");
    }

    const auto permissions = read_permissions(db, attempt_id, true);
    int64_t trust_revision = 1;
    bool reactivating = false;
    {
        Statement existing(db, "SELECT public_key,status,revision FROM trusted_devices WHERE uuid=?");
        existing.bind(1, finalization.peer_uuid);
        if (existing.row()) {
            require(existing.blob<32>(0) == finalization.peer_public_key,
                    "revoked trust identity does not match pairing attempt");
            require(existing.text(1) == "REVOKED", "active trust record already exists");
            const auto previous_revision = existing.integer(2);
            require(previous_revision > 0 && previous_revision < std::numeric_limits<int64_t>::max(),
                    "invalid trust revision for reactivation");
            trust_revision = previous_revision + 1;
            reactivating = true;
        }
    }

    TrustedDevice device;
    device.uuid = finalization.peer_uuid;
    device.public_key = finalization.peer_public_key;
    device.status = TrustStatus::Active;
    device.revision = trust_revision;
    device.local_alias = finalization.local_alias;
    device.peer_name = finalization.peer_name;
    device.peer_platform = finalization.peer_platform;
    device.pairing_method = finalization.pairing_method;
    device.paired_at = now;

    if (reactivating) {
        Statement reactivate(db, "UPDATE trusted_devices SET status=?3,revision=?4,local_alias=?5,peer_name=?6,"
                                 "peer_platform=?7,pairing_method=?8,paired_at=?9,revoked_at=?10,"
                                 "last_authenticated_at=?11 WHERE uuid=?1 AND public_key=?2 "
                                 "AND status='REVOKED' AND revision=?12");
        bind_device(reactivate, device);
        reactivate.bind(12, trust_revision - 1);
        reactivate.run();
        changed_one(db);
    } else {
        Statement insert(db, "INSERT INTO trusted_devices VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        bind_device(insert, device);
        insert.run();
    }
    Statement clear_permissions(db, "DELETE FROM device_permissions WHERE device_uuid=?");
    clear_permissions.bind(1, device.uuid);
    clear_permissions.run();
    write_permissions(db, device.uuid, permissions, false);

    const auto completion_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    require(completion_time >= 0, "system clock precedes Unix epoch");
    Statement consume(db, "DELETE FROM pairing_attempts "
                          "WHERE attempt_id=? AND revision=? AND status='CONFIRMED' "
                          "AND local_confirmed=1 AND peer_confirmed=1 AND expires_at>=?");
    consume.bind(1, attempt_id);
    consume.bind(2, expected_revision);
    consume.bind(3, completion_time);
    consume.run();
    changed_one(db);
    transaction.commit();
}

TrustStore::Configuration TrustStore::configuration() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {pragma_integer(impl_->db.get(), "PRAGMA user_version"),
            pragma_integer(impl_->db.get(), "PRAGMA foreign_keys") == 1,
            pragma_text(impl_->db.get(), "PRAGMA journal_mode"),
            pragma_integer(impl_->db.get(), "PRAGMA synchronous")};
}

} // namespace gasoline
