#include "core/identity/device_identity.hpp"
#include "core/utils/device_id.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
struct FsyncTrace;
thread_local FsyncTrace* active_fsync_trace = nullptr;

struct FsyncTrace {
    std::vector<struct stat> successful;
    struct stat failure_target{};
    bool inject_failure = false;
    size_t failures = 0;

    FsyncTrace() { active_fsync_trace = this; }
    ~FsyncTrace() { active_fsync_trace = nullptr; }
    FsyncTrace(const FsyncTrace&) = delete;
    FsyncTrace& operator=(const FsyncTrace&) = delete;
};

bool same_inode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}
} // namespace

extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
    auto* trace = active_fsync_trace;
    if (!trace) return __real_fsync(fd);
    struct stat info{};
    if (::fstat(fd, &info) != 0) return -1;
    if (trace->inject_failure && same_inode(info, trace->failure_target)) {
        ++trace->failures;
        errno = EIO;
        return -1;
    }
    const int result = __real_fsync(fd);
    if (result == 0) trace->successful.push_back(info);
    return result;
}

namespace {
using gasoline::DeviceIdentity;
namespace fs = std::filesystem;
constexpr const char* LEGACY_UUID = "12345678-1234-4234-8234-123456789abc";
constexpr size_t KEY_SIZE = 152;
constexpr size_t PUBLIC_OFFSET = 56;
constexpr size_t PRIVATE_OFFSET = 88;
using Signature = std::array<unsigned char, crypto_sign_BYTES>;

static_assert(!std::is_copy_constructible_v<DeviceIdentity>);
static_assert(std::is_same_v<decltype(std::declval<const DeviceIdentity&>().private_key()),
                             const DeviceIdentity::PrivateKey&>);
static_assert(std::is_same_v<decltype(gasoline::get_my_device_identity()), const DeviceIdentity&>);
static_assert(std::is_same_v<decltype(gasoline::get_my_device_id()), std::string>);

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryIdentity {
    fs::path root;
    fs::path path;
    TemporaryIdentity() {
        char pattern[] = "/tmp/gasoline-identity-tests-XXXXXX";
        const auto* directory = ::mkdtemp(pattern);
        require(directory != nullptr, "mkdtemp failed");
        root = directory;
        path = root / "storage" / "device_id";
    }
    ~TemporaryIdentity() {
        std::error_code ignored;
        fs::permissions(path.parent_path(), fs::perms::owner_all, ignored);
        fs::remove_all(root, ignored);
    }
    fs::path keys() const { return path.string() + ".keys"; }
};

std::string read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "cannot read test fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "cannot write test fixture");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    require(!output.fail() && ::chmod(path.c_str(), 0600) == 0, "fixture write failed");
}

mode_t mode(const fs::path& path) {
    struct stat info{};
    require(::stat(path.c_str(), &info) == 0 && info.st_uid == geteuid(), "stat/ownership failed");
    return info.st_mode & 07777;
}

void reject(const fs::path& path, const std::string& context) {
    // Retrying must not repair damaged material by generating a new identity.
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool rejected = false;
        try {
            const auto identity = DeviceIdentity::load_or_create(path);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("Device identity error:") == 0;
        }
        require(rejected, context + " did not produce a controlled identity failure");
    }
}

template<size_t N>
std::string hex(const std::array<unsigned char, N>& bytes) {
    std::array<char, N * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
    return encoded.data();
}

Signature sign_and_verify(const DeviceIdentity& identity) {
    constexpr unsigned char message[] = "Gasoline local identity persistence test";
    Signature signature{};
    unsigned long long size = 0;
    require(crypto_sign_detached(signature.data(), &size, message, sizeof(message),
                                 identity.private_key().data()) == 0 && size == signature.size(),
            "local signing failed");
    require(crypto_sign_verify_detached(signature.data(), message, sizeof(message),
                                        identity.public_key().data()) == 0, "local signature invalid");
    return signature;
}

void wait_success(pid_t child) {
    require(child >= 0, "fork failed");
    int status = 0;
    pid_t result;
    do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    require(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child process failed");
}

void creation_and_reload() {
    TemporaryIdentity storage;
    const auto first = DeviceIdentity::load_or_create(storage.path);
    require(first.device_id().size() == 36 && first.device_id()[14] == '4' &&
            std::string("89ab").find(first.device_id()[19]) != std::string::npos, "invalid UUID v4");
    require(mode(storage.path.parent_path()) == 0700, "directory is not user-only");
    for (const auto& path : {storage.path, storage.keys(), fs::path(storage.path.string() + ".lock")}) {
        require(mode(path) == 0600, "identity storage permissions are not 0600");
    }
    const auto manifest = read_bytes(storage.path);
    const auto keys = read_bytes(storage.keys());
    require(manifest == first.device_id() + "\ned25519-v1\n", "incorrect identity marker");
    require(keys.size() == KEY_SIZE && keys.substr(0, 20) == "GASOLINE-ED25519-V1\n" &&
            keys.substr(20, 36) == first.device_id(), "incorrect key record format/binding");
    require(sodium_memcmp(keys.data() + PUBLIC_OFFSET, first.public_key().data(), 32) == 0 &&
            sodium_memcmp(keys.data() + PRIVATE_OFFSET, first.private_key().data(), 64) == 0,
            "persisted key material differs from API");
    for (int i = 0; i < 4; ++i) {
        const auto loaded = DeviceIdentity::load_or_create(storage.path);
        require(first.device_id() == loaded.device_id() && first.public_key() == loaded.public_key() &&
                first.private_key() == loaded.private_key(), "reload changed identity");
    }
    require(read_bytes(storage.path) == manifest && read_bytes(storage.keys()) == keys, "reload rewrote storage");
    sign_and_verify(first);

    TemporaryIdentity independent;
    const auto other = DeviceIdentity::load_or_create(independent.path);
    require(other.device_id() != first.device_id() && other.public_key() != first.public_key(),
            "independent installations share an identity");
}

void require_directory_syncs(const FsyncTrace& trace, fs::path directory, bool published) {
    size_t index = 0;
    struct stat identity_directory{};
    require(::stat(directory.c_str(), &identity_directory) == 0, "directory stat failed");
    for (;;) {
        struct stat expected{};
        require(::stat(directory.c_str(), &expected) == 0, "ancestor stat failed");
        require(index < trace.successful.size() && S_ISDIR(trace.successful[index].st_mode) &&
                same_inode(trace.successful[index], expected), "missing/out-of-order ancestor fsync");
        ++index;
        if (directory == directory.root_path()) break;
        directory = directory.parent_path();
    }
    if (published) {
        // Both atomic publications must still flush the file, then its directory.
        for (int publication = 0; publication < 2; ++publication) {
            require(index + 1 < trace.successful.size() && S_ISREG(trace.successful[index].st_mode) &&
                    same_inode(trace.successful[index + 1], identity_directory),
                    "atomic publication did not fsync file then directory");
            index += 2;
        }
    }
    require(index == trace.successful.size(), "unexpected fsync sequence");
}

void complete_directory_tree_durability() {
    // Include completely absent parents and a tree left by an interrupted attempt.
    for (int existing_levels = 0; existing_levels <= 3; ++existing_levels) {
        TemporaryIdentity storage;
        const auto home = storage.root / "home";
        const auto config = home / ".config";
        storage.path = config / "gasoline" / "device_id";
        if (existing_levels == 1) fs::create_directories(home);
        if (existing_levels == 2) fs::create_directories(config);
        if (existing_levels == 3) fs::create_directories(storage.path.parent_path());
        require(fs::exists(home) == (existing_levels >= 1) &&
                fs::exists(config) == (existing_levels >= 2) &&
                fs::exists(storage.path.parent_path()) == (existing_levels >= 3), "invalid tree fixture");

        FsyncTrace trace;
        const auto identity = DeviceIdentity::load_or_create(storage.path);
        require_directory_syncs(trace, storage.path.parent_path(), true);
        require(mode(storage.path.parent_path()) == 0700 && mode(storage.path) == 0600 &&
                mode(storage.keys()) == 0600, "tree creation weakened permissions");
        trace.successful.clear();
        const auto loaded = DeviceIdentity::load_or_create(storage.path);
        require_directory_syncs(trace, storage.path.parent_path(), false);
        require(identity.device_id() == loaded.device_id() && identity.public_key() == loaded.public_key() &&
                identity.private_key() == loaded.private_key(), "directory sync changed identity");
    }
}

void directory_sync_failure() {
    TemporaryIdentity storage;
    storage.path = storage.root / "home" / ".config" / "gasoline" / "device_id";
    FsyncTrace trace;
    require(::stat(storage.root.c_str(), &trace.failure_target) == 0, "failure target stat failed");
    trace.inject_failure = true;
    reject(storage.path, "ancestor fsync failure");
    require(trace.failures == 2 && !fs::exists(storage.path) && !fs::exists(storage.keys()),
            "ancestor fsync failure allowed identity publication");

    // The failed attempt created the whole tree; retry must still sync its parents.
    trace.inject_failure = false;
    trace.successful.clear();
    const auto identity = DeviceIdentity::load_or_create(storage.path);
    require_directory_syncs(trace, storage.path.parent_path(), true);
    const auto manifest = read_bytes(storage.path);
    const auto keys = read_bytes(storage.keys());
    trace.inject_failure = true;
    reject(storage.path, "ancestor fsync failure on reload");
    require(trace.failures == 4 && read_bytes(storage.path) == manifest && read_bytes(storage.keys()) == keys,
            "ancestor fsync failure modified established identity");
}

void process_restart() {
    TemporaryIdentity storage;
    std::string uuid, public_key, signature;
    {
        const auto original = DeviceIdentity::load_or_create(storage.path);
        uuid = original.device_id();
        public_key = hex(original.public_key());
        signature = hex(sign_and_verify(original));
    }
    const auto before = read_bytes(storage.keys());
    for (int restart = 0; restart < 2; ++restart) {
        const pid_t child = ::fork();
        if (child == 0) {
            // Exec discards all inherited identity objects and libsodium state.
            // Only public information is passed in argv, never private key bytes.
            ::execl("/proc/self/exe", "identity_tests", "--reload", storage.path.c_str(),
                    uuid.c_str(), public_key.c_str(), signature.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        wait_success(child);
    }
    require(read_bytes(storage.keys()) == before, "restart rewrote keys");
}

void legacy_migration() {
    TemporaryIdentity storage;
    write_bytes(storage.path, std::string(LEGACY_UUID) + "\n");
    require(::chmod(storage.path.parent_path().c_str(), 0755) == 0, "chmod failed");
    const auto migrated = DeviceIdentity::load_or_create(storage.path);
    require(migrated.device_id() == LEGACY_UUID, "migration changed existing UUID");
    require(mode(storage.path.parent_path()) == 0700, "legacy directory was not secured");
    const auto loaded = DeviceIdentity::load_or_create(storage.path);
    require(loaded.private_key() == migrated.private_key(), "migration generated keys twice");
    fs::remove(storage.keys());
    reject(storage.path, "lost migrated keys");
    require(!fs::exists(storage.keys()), "lost migrated keys were regenerated");
}

void missing_established_material() {
    for (bool remove_uuid : {false, true}) {
        TemporaryIdentity storage;
        const auto identity = DeviceIdentity::load_or_create(storage.path);
        const auto missing = remove_uuid ? storage.path : storage.keys();
        const auto surviving = remove_uuid ? storage.keys() : storage.path;
        const auto before = read_bytes(surviving);
        require(fs::remove(missing), "fixture removal failed");
        reject(storage.path, "missing established material");
        require(!fs::exists(missing) && read_bytes(surviving) == before, "missing material regenerated");
    }
}

void corrupt_keys() {
    TemporaryIdentity storage;
    const auto identity = DeviceIdentity::load_or_create(storage.path);
    const auto original = read_bytes(storage.keys());
    std::vector<std::string> damaged;
    for (size_t size : {size_t(0), size_t(1), size_t(55), size_t(88), KEY_SIZE - 1}) {
        damaged.push_back(original.substr(0, size));
    }
    damaged.push_back(original + "extra");
    for (size_t offset : {size_t(0), size_t(20), PUBLIC_OFFSET, PRIVATE_OFFSET, PRIVATE_OFFSET + 32}) {
        auto value = original;
        value[offset] ^= 1;
        damaged.push_back(value);
    }
    for (const auto& value : damaged) {
        write_bytes(storage.keys(), value);
        reject(storage.path, "corrupt/truncated key record");
        require(read_bytes(storage.keys()) == value, "corrupt keys were replaced");
        require(read_bytes(storage.path) == identity.device_id() + "\ned25519-v1\n", "UUID changed");
    }
    write_bytes(storage.keys(), original);
    const auto restored = DeviceIdentity::load_or_create(storage.path);
    require(restored.private_key() == identity.private_key(), "valid backup did not restore identity");
}

void corrupt_manifest() {
    TemporaryIdentity storage;
    const auto identity = DeviceIdentity::load_or_create(storage.path);
    const auto keys = read_bytes(storage.keys());
    for (const std::string value : {std::string(), std::string("not-a-uuid"), identity.device_id(),
                                   identity.device_id() + "\ned25519-v2\n", std::string(4096, 'x'),
                                   std::string(LEGACY_UUID) + "\ned25519-v1\n"}) {
        write_bytes(storage.path, value);
        reject(storage.path, "invalid/mismatched manifest");
        require(read_bytes(storage.path) == value && read_bytes(storage.keys()) == keys, "bad UUID regenerated");
    }
    TemporaryIdentity legacy;
    write_bytes(legacy.path, "broken legacy UUID\n");
    reject(legacy.path, "corrupt legacy UUID");
    require(!fs::exists(legacy.keys()), "corrupt legacy UUID received new keys");
}

void unsafe_permissions() {
    TemporaryIdentity storage;
    const auto identity = DeviceIdentity::load_or_create(storage.path);
    const auto original = read_bytes(storage.keys());
    for (const auto& path : {storage.keys(), storage.path, fs::path(storage.path.string() + ".lock")}) {
        for (mode_t permissions : {0600 | 0044, 0000, 0400}) {
            require(::chmod(path.c_str(), permissions) == 0, "chmod failed");
            reject(storage.path, "unsafe/unreadable file permissions");
            require(::chmod(path.c_str(), 0600) == 0, "chmod restore failed");
        }
    }
    if (::geteuid() != 0) {
        require(::chmod(storage.path.parent_path().c_str(), 0000) == 0, "chmod failed");
        reject(storage.path, "inaccessible directory");
        require(::chmod(storage.path.parent_path().c_str(), 0700) == 0, "chmod restore failed");
    }
    require(read_bytes(storage.keys()) == original, "permission errors changed keys");
}

void unsafe_file_types() {
    TemporaryIdentity storage;
    const auto identity = DeviceIdentity::load_or_create(storage.path);
    const auto original = read_bytes(storage.keys());
    const auto backup = storage.root / "backup";
    fs::rename(storage.keys(), backup);
    for (const auto& target : {backup, storage.root / "missing"}) {
        fs::create_symlink(target, storage.keys());
        reject(storage.path, "symlinked key record");
        require(fs::is_symlink(storage.keys()), "symlink replaced");
        fs::remove(storage.keys());
    }
    fs::create_hard_link(backup, storage.keys());
    reject(storage.path, "hard-linked key record");
    fs::remove(storage.keys());
    fs::create_directory(storage.keys());
    reject(storage.path, "directory key record");
    fs::remove(storage.keys());
    require(::mkfifo(storage.keys().c_str(), 0600) == 0, "mkfifo failed");
    reject(storage.path, "FIFO key record");
    fs::remove(storage.keys());
    fs::rename(backup, storage.keys());
    fs::create_directory_symlink(storage.path.parent_path(), storage.root / "alias");
    reject(storage.root / "alias" / "device_id", "symlinked identity directory");
    require(read_bytes(storage.keys()) == original, "unsafe paths modified key material");
}

void interrupted_initialization() {
    for (bool marked : {false, true}) {
        for (const std::string suffix : {".tmp", ".keys.tmp"}) {
            TemporaryIdentity storage;
            if (marked) write_bytes(storage.path, std::string(LEGACY_UUID) + "\ned25519-v1\n");
            const fs::path temporary = storage.path.string() + suffix;
            write_bytes(temporary, "partial write");
            reject(storage.path, "interrupted initialization");
            require(read_bytes(temporary) == "partial write" && !fs::exists(storage.keys()), "partial file repaired");
            require(fs::exists(storage.path) == marked, "partial initialization regenerated UUID");
        }
    }
    TemporaryIdentity marker_only;
    write_bytes(marker_only.path, std::string(LEGACY_UUID) + "\ned25519-v1\n");
    reject(marker_only.path, "crash between manifest and key creation");
    require(!fs::exists(marker_only.keys()), "marker-only identity received replacement keys");

    TemporaryIdentity established;
    const auto identity = DeviceIdentity::load_or_create(established.path);
    const auto keys = read_bytes(established.keys());
    write_bytes(established.keys().string() + ".tmp", keys.substr(0, 100));
    reject(established.path, "partial file beside established identity");
    require(read_bytes(established.keys()) == keys, "partial file replaced established keys");
}

void concurrent_initialization() {
    TemporaryIdentity storage;
    std::vector<std::future<std::string>> initializers;
    for (int i = 0; i < 8; ++i) {
        initializers.push_back(std::async(std::launch::async, [&] {
            const auto identity = DeviceIdentity::load_or_create(storage.path);
            return identity.device_id() + hex(identity.public_key()) + hex(sign_and_verify(identity));
        }));
    }
    const auto expected = initializers.front().get();
    for (size_t i = 1; i < initializers.size(); ++i) {
        require(initializers[i].get() == expected, "concurrent initialization produced different identities");
    }
}

void abandoned_lock() {
    TemporaryIdentity storage;
    const auto identity = DeviceIdentity::load_or_create(storage.path);
    const fs::path lock = storage.path.string() + ".lock";
    const pid_t child = ::fork();
    if (child == 0) {
        const int fd = ::open(lock.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) ::_exit(1);
        // Exit without explicit unlock/close, as an interrupted process would.
        ::_exit(0);
    }
    wait_success(child);
    const auto loaded = DeviceIdentity::load_or_create(storage.path);
    require(loaded.private_key() == identity.private_key(), "abandoned lock blocked/changed identity");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string(argv[1]) == "--reload") {
            const auto identity = DeviceIdentity::load_or_create(argv[2]);
            require(identity.device_id() == argv[3] && hex(identity.public_key()) == argv[4] &&
                    hex(sign_and_verify(identity)) == argv[5], "fresh process identity mismatch");
            const auto bytes = read_bytes(std::string(argv[2]) + ".keys");
            require(bytes.size() == KEY_SIZE &&
                    sodium_memcmp(bytes.data() + PRIVATE_OFFSET, identity.private_key().data(), 64) == 0,
                    "fresh process private key mismatch");
            return 0;
        }
        require(argc == 1, "unexpected test arguments");
        const std::pair<const char*, void (*)()> tests[] = {
            {"creation, permissions, exact reload, independent identities", creation_and_reload},
            {"complete directory tree durability and publication order", complete_directory_tree_durability},
            {"ancestor fsync failures and retry durability", directory_sync_failure},
            {"fresh-process restarts", process_restart},
            {"legacy UUID migration", legacy_migration},
            {"missing established material", missing_established_material},
            {"corrupt and partial key records", corrupt_keys},
            {"corrupt and mismatched manifest", corrupt_manifest},
            {"unsafe permissions and inaccessible storage", unsafe_permissions},
            {"unsafe file types and links", unsafe_file_types},
            {"interrupted initialization fails closed", interrupted_initialization},
            {"concurrent initialization", concurrent_initialization},
            {"abandoned process lock", abandoned_lock},
        };
        for (const auto& test : tests) {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
