#include "core/trust/trust_store.hpp"
#include "core/trust/trust_schema.hpp"

#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace gasoline;
namespace fs = std::filesystem;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const TrustStoreError&) { rejected = true; }
    require(rejected, "expected a controlled TrustStoreError");
}

struct Fixture {
    fs::path root;
    fs::path path;
    Fixture() {
        char name[] = "/tmp/gasoline-trust-tests-XXXXXX";
        const auto* created = ::mkdtemp(name);
        require(created != nullptr, "mkdtemp failed");
        root = created;
        path = root / "home" / ".local" / "state" / "gasoline" / "trust.sqlite3";
    }
    ~Fixture() {
        std::error_code ignored;
        fs::permissions(path.parent_path(), fs::perms::owner_all, ignored);
        fs::remove_all(root, ignored);
    }
};

std::string bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "fixture read failed");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    file.close();
    require(!file.fail() && ::chmod(path.c_str(), 0600) == 0, "fixture write failed");
}

mode_t mode(const fs::path& path) {
    struct stat info{};
    require(::lstat(path.c_str(), &info) == 0 && info.st_uid == geteuid(), "fixture stat/ownership failed");
    return info.st_mode & 07777;
}

struct RawDb {
    sqlite3* db = nullptr;
    explicit RawDb(const fs::path& path) {
        require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "fixture SQLite open failed");
        run("PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000");
    }
    ~RawDb() { sqlite3_close(db); }
    void run(const std::string& sql) {
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("fixture SQL failed: ") + sqlite3_errmsg(db));
    }
    void constraint(const std::string& sql) {
        const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
        require((result & 255) == SQLITE_CONSTRAINT, "SQLite did not enforce constraint");
    }
    int scalar(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "fixture prepare failed");
        const int result = sqlite3_step(statement);
        const int value = sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
        require(result == SQLITE_ROW, "fixture scalar failed");
        return value;
    }
};

TrustUuid id(unsigned value) {
    TrustUuid result{};
    result[0] = 0xa1;
    result[6] = 0x40;
    result[8] = 0x80;
    result[14] = static_cast<unsigned char>(value >> 8);
    result[15] = static_cast<unsigned char>(value);
    return result;
}

TrustedDevice device(unsigned value = 1) {
    TrustedDevice d;
    d.uuid = id(value);
    d.public_key[0] = 0x31;
    d.public_key[30] = static_cast<unsigned char>(value >> 8);
    d.public_key[31] = static_cast<unsigned char>(value);
    d.local_alias = "Local alias";
    d.peer_name = "Fixture peer";
    d.peer_platform = "test";
    d.pairing_method = "fixture";
    d.paired_at = 100;
    d.permissions = {"messages.receive", "notifications.receive"};
    return d;
}

PairingAttempt attempt(unsigned value = 1) {
    PairingAttempt a;
    a.attempt_id = id(value + 1000);
    a.peer_uuid = id(value);
    a.peer_public_key = device(value).public_key;
    a.created_at = 100;
    a.updated_at = 100;
    a.expires_at = 200;
    a.proposed_permissions = {"messages.receive"};
    return a;
}

void fresh_and_configuration() {
    Fixture f;
    const mode_t previous = ::umask(0);
    try {
        require(sqlite3_threadsafe() != 0 && sqlite3_libversion_number() >= 3037000,
                "test environment SQLite must be thread-safe >= 3.37.0");
        TrustStore store(f.path);
        const auto c = store.configuration();
        require(c.schema_version == 2 && c.foreign_keys && c.journal_mode == "wal" && c.synchronous == 2,
                "incorrect SQLite configuration");
        require(store.state(id(1)) == TrustState::Unknown && !store.find_device(id(1)) &&
                store.permissions(id(1)).empty(), "fresh store contains trust");
        store.insert_device(device());
        require(mode(f.path.parent_path()) == 0700, "state directory is not 0700");
        for (const auto* suffix : {"", ".lock", "-wal", "-shm"})
            require(mode(f.path.string() + suffix) == 0600, "database/sidecar is not 0600 under permissive umask");
        RawDb raw(f.path);
        require(raw.scalar("SELECT count(*) FROM sqlite_schema WHERE type='table'") == 4, "unexpected schema tables");
        require(raw.scalar("PRAGMA application_id") == trust_schema::APPLICATION_ID, "missing application id");
    } catch (...) { ::umask(previous); throw; }
    ::umask(previous);
}

void default_state_location() {
    Fixture f;
    const auto* previous_value = std::getenv("XDG_STATE_HOME");
    const std::optional<std::string> previous = previous_value ? std::optional<std::string>(previous_value) : std::nullopt;
    const auto restore = [&] {
        if (previous) ::setenv("XDG_STATE_HOME", previous->c_str(), 1);
        else ::unsetenv("XDG_STATE_HOME");
    };
    try {
        require(::unsetenv("XDG_STATE_HOME") == 0, "unsetenv failed");
        const auto fallback = TrustStore::default_database_path();
        require(fallback.is_absolute() && fallback.parent_path().filename() == "gasoline" &&
                fallback.parent_path().parent_path().filename() == "state", "incorrect default state location");
        std::vector<std::future<fs::path>> lookups;
        for (int i = 0; i < 8; ++i) lookups.push_back(std::async(std::launch::async, [] {
            return TrustStore::default_database_path();
        }));
        for (auto& lookup : lookups) require(lookup.get() == fallback, "concurrent default path lookup changed");
        const auto state = f.root / "custom-state";
        require(::setenv("XDG_STATE_HOME", state.c_str(), 1) == 0, "setenv failed");
        require(TrustStore::default_database_path() == state / "gasoline" / "trust.sqlite3", "XDG state override ignored");
        require(::setenv("XDG_STATE_HOME", "relative-state", 1) == 0, "setenv failed");
        require(TrustStore::default_database_path() == fallback, "relative XDG state override accepted");
    } catch (...) { restore(); throw; }
    restore();
}

void binary_validation() {
    const auto value = trust_uuid_from_string("A1234567-89AB-4CDE-8123-456789abcdef");
    require(trust_uuid_to_string(value) == "a1234567-89ab-4cde-8123-456789abcdef", "UUID binary roundtrip failed");
    for (const auto* bad : {"", "short", "g1234567-89ab-4cde-8123-456789abcdef", "a1234567_89ab-4cde-8123-456789abcdef"})
        rejects([&] { trust_uuid_from_string(bad); });
    for (size_t size : {size_t(0), size_t(31), size_t(33), size_t(64)})
        rejects([&] { trust_public_key_from_bytes(std::string(size, 'k')); });
    const auto key = trust_public_key_from_bytes(std::string(32, '\0'));
    require(key == TrustPublicKey{}, "binary public-key parser rejected embedded zero bytes");
}

void device_lifecycle() {
    Fixture f;
    TrustStore store(f.path);
    auto d = device();
    store.insert_device(d);
    auto found = store.find_device(d.uuid);
    require(found && found->public_key == d.public_key && found->permissions == d.permissions &&
            found->local_alias == d.local_alias && found->paired_at == d.paired_at, "insert/fetch mismatch");
    require(store.find_by_public_key(d.public_key)->uuid == d.uuid && store.state(d.uuid) == TrustState::Active,
            "public-key lookup/active state failed");
    d.revision = 2;
    d.local_alias = "Renamed locally";
    d.last_authenticated_at = 110;
    d.permissions = {"files.receive"};
    store.update_device(d, 1);
    found = store.find_device(d.uuid);
    require(found->revision == 2 && found->local_alias == d.local_alias && found->last_authenticated_at == 110 &&
            found->permissions == d.permissions, "atomic update failed");
    rejects([&] { store.update_device(d, 1); });
    rejects([&] { store.revoke_device(d.uuid, 1, 120); });
    store.revoke_device(d.uuid, 2, 120);
    found = store.find_device(d.uuid);
    require(found->status == TrustStatus::Revoked && found->revision == 3 && found->revoked_at == 120 &&
            store.state(d.uuid) == TrustState::Revoked && store.permissions(d.uuid).empty(), "revocation failed");
    store.create_attempt(attempt());
    require(store.state(d.uuid) == TrustState::Revoked, "pending record resurrected revoked device");
    rejects([&] { store.insert_device(device()); });
    rejects([&] { store.revoke_device(id(99), 1, 120); });
    auto reactivated = *found;
    reactivated.status = TrustStatus::Active;
    reactivated.revoked_at.reset();
    reactivated.revision = 4;
    reactivated.permissions = {"files.receive"};
    store.update_device(reactivated, 3);
    require(store.state(d.uuid) == TrustState::Active && store.find_device(d.uuid)->revision == 4 &&
            store.permissions(d.uuid) == reactivated.permissions, "explicit revision-checked reactivation failed");
}

void duplicates_and_constraints() {
    Fixture f;
    TrustStore store(f.path);
    store.insert_device(device());
    auto duplicate_uuid = device(2);
    duplicate_uuid.uuid = id(1);
    rejects([&] { store.insert_device(duplicate_uuid); });
    auto duplicate_key = device(2);
    duplicate_key.public_key = device().public_key;
    rejects([&] { store.insert_device(duplicate_key); });
    store.revoke_device(id(1), 1, 120);
    rejects([&] { store.insert_device(duplicate_key); });
    RawDb raw(f.path);
    for (const auto* sql : {
        "UPDATE trusted_devices SET uuid=zeroblob(15)",
        "UPDATE trusted_devices SET uuid='0123456789abcdef'",
        "UPDATE trusted_devices SET public_key=zeroblob(31)",
        "UPDATE trusted_devices SET status='PENDING'",
        "UPDATE trusted_devices SET revision=0",
        "UPDATE trusted_devices SET revoked_at=NULL",
        "INSERT INTO device_permissions VALUES(zeroblob(16),'messages.receive')"}) raw.constraint(sql);
    store.create_attempt(attempt(2));
    for (const auto* sql : {
        "UPDATE pairing_attempts SET peer_uuid=zeroblob(17)",
        "UPDATE pairing_attempts SET peer_public_key=zeroblob(64)",
        "UPDATE pairing_attempts SET status='ACTIVE'",
        "UPDATE pairing_attempts SET status='CONFIRMED'",
        "UPDATE pairing_attempts SET local_confirmed=2",
        "INSERT INTO pairing_attempt_permissions VALUES(zeroblob(16),'messages.receive')",
        "INSERT INTO pairing_attempt_permissions SELECT * FROM pairing_attempt_permissions"}) raw.constraint(sql);
    store.delete_attempt(attempt(2).attempt_id, 1);
    require(raw.scalar("SELECT count(*) FROM pairing_attempt_permissions") == 0, "attempt permissions did not cascade");
}

void attempts_and_pending_isolation() {
    Fixture f;
    TrustStore store(f.path);
    auto a = attempt();
    store.create_attempt(a);
    rejects([&] { store.create_attempt(a); });
    auto found = store.find_attempt(a.attempt_id);
    require(found && found->peer_public_key == a.peer_public_key && found->proposed_permissions == a.proposed_permissions,
            "attempt insert/fetch mismatch");
    require(store.state(a.peer_uuid) == TrustState::PairingPending && !store.find_device(a.peer_uuid) &&
            !store.find_by_public_key(a.peer_public_key) && store.permissions(a.peer_uuid).empty(), "pending became trusted");
    a.revision = 2;
    a.local_confirmed = true;
    a.updated_at = 110;
    store.update_attempt(a, 1);
    require(store.find_attempt(a.attempt_id)->local_confirmed, "confirmation state not persisted");
    rejects([&] { store.update_attempt(a, 1); });
    rejects([&] { store.delete_attempt(a.attempt_id, 1); });
    a.peer_confirmed = true;
    a.status = PairingAttemptStatus::Confirmed;
    a.revision = 3;
    store.update_attempt(a, 2);
    require(store.state(a.peer_uuid) == TrustState::PairingPending && store.permissions(a.peer_uuid).empty(),
            "confirmation flags granted trust");
    a.status = PairingAttemptStatus::Cancelled;
    a.revision = 4;
    store.update_attempt(a, 3);
    require(store.state(a.peer_uuid) == TrustState::Unknown, "cancelled attempt remains pending");
    store.delete_attempt(a.attempt_id, 4);
    require(!store.find_attempt(a.attempt_id), "attempt deletion failed");
}

void transactional_rollback() {
    Fixture f;
    TrustStore store(f.path);
    auto d = device();
    d.permissions = {"duplicate", "duplicate"};
    rejects([&] { store.insert_device(d); });
    require(!store.find_device(d.uuid), "failed permission insert left trusted row behind");
    store.insert_device(device());
    d.revision = 2;
    d.local_alias = "Must roll back";
    rejects([&] { store.update_device(d, 1); });
    const auto original = store.find_device(d.uuid);
    require(original->revision == 1 && original->local_alias == device().local_alias &&
            original->permissions == device().permissions, "failed update did not roll back row and permissions");
    d = device();
    d.revision = 2;
    d.public_key = device(2).public_key;
    rejects([&] { store.update_device(d, 1); });
    d = device();
    d.revision = std::numeric_limits<int64_t>::max();
    rejects([&] { store.update_device(d, std::numeric_limits<int64_t>::max()); });
    auto a = attempt();
    a.proposed_permissions = {"duplicate", "duplicate"};
    rejects([&] { store.create_attempt(a); });
    require(!store.find_attempt(a.attempt_id), "failed attempt insert did not roll back");
    store.create_attempt(attempt());
    a.revision = 2;
    a.local_confirmed = true;
    rejects([&] { store.update_attempt(a, 1); });
    require(store.find_attempt(a.attempt_id)->revision == 1 && !store.find_attempt(a.attempt_id)->local_confirmed,
            "failed attempt update did not roll back");
}

void reopen_and_wal_recovery() {
    Fixture f;
    {
        TrustStore store(f.path);
        store.insert_device(device());
        store.create_attempt(attempt(2));
        store.revoke_device(id(1), 1, 120);
    }
    {
        TrustStore store(f.path);
        require(store.find_device(id(1))->revision == 2 && store.state(id(1)) == TrustState::Revoked &&
                store.find_attempt(attempt(2).attempt_id)->peer_uuid == id(2), "reopen lost state");
    }
    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        try {
            TrustStore store(f.path);
            store.insert_device(device(3));
            ::_exit(0); // Committed WAL survives without SQLite's normal close/checkpoint.
        } catch (...) { ::_exit(1); }
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "WAL writer failed");
    require(fs::exists(f.path.string() + "-wal"), "fixture did not retain WAL");
    TrustStore recovered(f.path);
    require(recovered.find_device(id(3)).has_value() && recovered.state(id(1)) == TrustState::Revoked,
            "WAL recovery lost committed trust");
}

void migration(bool fail) {
    Fixture f;
    write(f.path, "");
    {
        RawDb old(f.path);
        old.run(trust_schema::V1);
        old.run("PRAGMA application_id=" + std::to_string(trust_schema::APPLICATION_ID) + "; PRAGMA user_version=1");
        old.run("INSERT INTO trusted_devices VALUES(zeroblob(16),zeroblob(32),'REVOKED',7,'alias','peer','linux','fixture',100,110,NULL)");
        if (fail) old.run("CREATE TABLE pairing_attempt_permissions(blocker INTEGER)");
    }
    if (fail) {
        for (int retry = 0; retry < 2; ++retry) rejects([&] { TrustStore failed(f.path); });
        RawDb unchanged(f.path);
        require(unchanged.scalar("PRAGMA user_version") == 1 &&
                unchanged.scalar("SELECT count(*) FROM sqlite_schema WHERE name='pairing_attempts'") == 0 &&
                unchanged.scalar("SELECT revision FROM trusted_devices") == 7, "failed migration partially committed");
    } else {
        TrustStore upgraded(f.path);
        require(upgraded.configuration().schema_version == 2 && upgraded.find_device(TrustUuid{})->revision == 7 &&
                upgraded.state(TrustUuid{}) == TrustState::Revoked, "migration lost existing trust");
        upgraded.create_attempt(attempt());
        require(upgraded.find_attempt(attempt().attempt_id).has_value(), "migrated attempt table unavailable");
    }
}

void corrupt_and_failed_open() {
    for (const auto& value : {std::string(), std::string("not a database"), std::string(4096, 'x')}) {
        Fixture f;
        write(f.path, value);
        for (int retry = 0; retry < 2; ++retry) rejects([&] { TrustStore failed(f.path); });
        require(bytes(f.path) == value, "corrupt/truncated database was replaced");
    }
    for (bool truncate : {false, true}) {
        Fixture f;
        { TrustStore store(f.path); store.insert_device(device()); store.revoke_device(id(1), 1, 120); }
        auto damaged = bytes(f.path);
        require(damaged.size() > 100, "missing established SQLite pages");
        if (truncate) damaged.resize(damaged.size() / 2);
        else damaged[100] ^= static_cast<char>(0xff); // Corrupt page one while retaining the SQLite header.
        write(f.path, damaged);
        for (int retry = 0; retry < 2; ++retry) rejects([&] { TrustStore failed(f.path); });
        require(bytes(f.path) == damaged, "damaged established trust database was replaced");
    }
    for (const std::string pragma : {"PRAGMA user_version=99", "PRAGMA application_id=0", "PRAGMA user_version=0"}) {
        Fixture f;
        { TrustStore store(f.path); store.insert_device(device()); }
        { RawDb raw(f.path); raw.run(pragma); }
        const auto before = bytes(f.path);
        rejects([&] { TrustStore failed(f.path); });
        require(bytes(f.path) == before, "unsupported schema/database was replaced");
    }
    Fixture orphan;
    write(orphan.path.string() + "-wal", "orphaned WAL");
    rejects([&] { TrustStore failed(orphan.path); });
    require(!fs::exists(orphan.path), "orphaned WAL caused new database creation");
    Fixture bad_parent;
    write(bad_parent.root / "file", "not a directory");
    rejects([&] { TrustStore failed(bad_parent.root / "file" / "trust.sqlite3"); });
}

void unsafe_storage() {
    Fixture f;
    { TrustStore store(f.path); store.insert_device(device()); }
    const auto original = bytes(f.path);
    require(::chmod(f.path.c_str(), 0644) == 0, "chmod failed");
    rejects([&] { TrustStore failed(f.path); });
    require(::chmod(f.path.c_str(), 0600) == 0, "chmod restore failed");
    const auto backup = f.root / "backup";
    fs::rename(f.path, backup);
    fs::create_symlink(backup, f.path);
    rejects([&] { TrustStore failed(f.path); });
    fs::remove(f.path);
    fs::create_hard_link(backup, f.path);
    rejects([&] { TrustStore failed(f.path); });
    fs::remove(f.path);
    fs::rename(backup, f.path);
    fs::create_directory_symlink(f.path.parent_path(), f.root / "alias");
    rejects([&] { TrustStore failed(f.root / "alias" / "trust.sqlite3"); });
    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
        const fs::path sidecar = f.path.string() + suffix;
        write(sidecar, "unsafe sidecar");
        require(::chmod(sidecar.c_str(), 0644) == 0, "chmod failed");
        rejects([&] { TrustStore failed(f.path); });
        fs::remove(sidecar);
        fs::create_symlink(f.root / "missing", sidecar);
        rejects([&] { TrustStore failed(f.path); });
        fs::remove(sidecar);
    }
    require(bytes(f.path) == original, "unsafe storage was modified");
    if (::geteuid() != 0) {
        require(::chmod(f.path.parent_path().c_str(), 0000) == 0, "chmod failed");
        rejects([&] { TrustStore failed(f.path); });
        require(::chmod(f.path.parent_path().c_str(), 0700) == 0, "chmod restore failed");
    }
}

void concurrent_access() {
    Fixture f;
    std::vector<std::future<void>> creators;
    for (int i = 0; i < 4; ++i) creators.push_back(std::async(std::launch::async, [&] { TrustStore store(f.path); }));
    for (auto& creator : creators) creator.get();
    TrustStore first(f.path), second(f.path);
    std::vector<std::future<void>> writers;
    for (unsigned thread = 0; thread < 8; ++thread) {
        writers.push_back(std::async(std::launch::async, [&, thread] {
            auto& store = thread % 2 ? first : second;
            for (unsigned i = 0; i < 10; ++i) {
                const auto d = device(thread * 10 + i + 1);
                store.insert_device(d);
                require(store.find_device(d.uuid)->public_key == d.public_key, "concurrent read/write mismatch");
            }
        }));
    }
    for (auto& writer : writers) writer.get();
    for (unsigned i = 1; i <= 80; ++i) require(first.find_device(id(i)).has_value(), "concurrent insert lost");
    std::atomic<int> successes{0}, conflicts{0};
    auto update = [&](TrustStore& store) {
        auto d = device();
        d.revision = 2;
        try { store.update_device(d, 1); ++successes; }
        catch (const TrustStoreError&) { ++conflicts; }
    };
    auto a = std::async(std::launch::async, [&] { update(first); });
    auto b = std::async(std::launch::async, [&] { update(second); });
    a.get(); b.get();
    require(successes == 1 && conflicts == 1 && first.find_device(id(1))->revision == 2,
            "revision compare-and-swap allowed a lost update");
}
} // namespace

int main() {
    try {
        const std::pair<const char*, void (*)()> tests[] = {
            {"fresh schema, WAL/FULL/FK configuration and private files", fresh_and_configuration},
            {"default/XDG state location and concurrent path lookup", default_state_location},
            {"binary UUID/public-key validation", binary_validation},
            {"device lifecycle, revisions and revocation", device_lifecycle},
            {"duplicate identities, SQL checks and foreign keys", duplicates_and_constraints},
            {"attempt lifecycle and pending trust isolation", attempts_and_pending_isolation},
            {"transactional rollback and immutable identity", transactional_rollback},
            {"reopen and committed WAL recovery", reopen_and_wal_recovery},
            {"v1 to v2 schema migration", [] { migration(false); }},
            {"transactional migration failure", [] { migration(true); }},
            {"corruption, unsupported schema and open failure", corrupt_and_failed_open},
            {"unsafe permissions and storage paths", unsafe_storage},
            {"concurrent API access, initialization and revisions", concurrent_access},
        };
        for (const auto& test : tests) {
            test.second();
            std::cout << "PASS: " << test.first << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
