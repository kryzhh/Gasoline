#include "device_identity.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace gasoline {
namespace {

constexpr std::string_view IDENTITY_MARKER = "\ned25519-v1\n";
constexpr std::string_view KEY_MAGIC = "GASOLINE-ED25519-V1\n";
constexpr size_t UUID_SIZE = 36;
constexpr size_t KEY_FILE_SIZE = KEY_MAGIC.size() + UUID_SIZE +
                                 crypto_sign_PUBLICKEYBYTES + crypto_sign_SECRETKEYBYTES;

std::runtime_error make_error(const std::string& message) {
    return std::runtime_error("Device identity error: " + message);
}

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    int get() const { return fd_; }
    void close_checked() {
        if (::close(std::exchange(fd_, -1)) != 0) {
            throw make_error("failed to close persisted identity file");
        }
    }
private:
    int fd_;
};

template<size_t Size>
struct SecretBuffer {
    std::array<unsigned char, Size> bytes{};
    ~SecretBuffer() { sodium_memzero(bytes.data(), bytes.size()); }
    SecretBuffer() = default;
    SecretBuffer(const SecretBuffer&) = delete;
    SecretBuffer& operator=(const SecretBuffer&) = delete;
};

void initialize_sodium() {
    static const int result = sodium_init();
    if (result < 0) {
        throw make_error("libsodium initialization failed");
    }
}

std::filesystem::path resolve_home_directory() {
    const auto* entry = getpwuid(getuid());
    if (entry == nullptr || entry->pw_dir == nullptr || entry->pw_dir[0] == '\0') {
        throw make_error("unable to determine the current user's home directory");
    }
    return entry->pw_dir;
}

bool is_valid_uuid(std::string_view value) {
    if (value.size() != UUID_SIZE) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f') ||
                     (value[i] >= 'A' && value[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::string generate_uuid() {
    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    constexpr char hex[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(UUID_SIZE);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) uuid.push_back('-');
        uuid.push_back(hex[bytes[i] >> 4]);
        uuid.push_back(hex[bytes[i] & 0x0F]);
    }
    return uuid;
}

void sync_file(int fd);

void sync_directory_tree(int directory) {
    FileDescriptor current(::fcntl(directory, F_DUPFD_CLOEXEC, 0));
    if (current.get() < 0) throw make_error("unable to duplicate identity directory handle");
    // Existing entries may come from an interrupted attempt. Flush every
    // containing directory, following the opened tree rather than path strings.
    for (;;) {
        sync_file(current.get());
        FileDescriptor parent(::openat(current.get(), "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        struct stat current_info{}, parent_info{};
        if (parent.get() < 0 || ::fstat(current.get(), &current_info) != 0 ||
            ::fstat(parent.get(), &parent_info) != 0) {
            throw make_error("unable to inspect identity directory's ancestor");
        }
        if (current_info.st_dev == parent_info.st_dev && current_info.st_ino == parent_info.st_ino) return;
        current = std::move(parent);
    }
}

FileDescriptor open_identity_directory(const std::filesystem::path& parent) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) throw make_error("failed to create identity directory: " + error.message());
    FileDescriptor directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (directory.get() < 0 || ::fstat(directory.get(), &info) != 0 || info.st_uid != geteuid()) {
        throw make_error("identity directory is inaccessible, symlinked, or not owned by this user");
    }
    if (::fchmod(directory.get(), 0700) != 0) {
        throw make_error("failed to secure identity directory permissions");
    }
    sync_directory_tree(directory.get());
    return directory;
}

bool entry_exists(int directory, const std::string& name) {
    struct stat info{};
    if (::fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) return true;
    if (errno == ENOENT) return false;
    throw make_error("unable to inspect " + name + ": " + std::strerror(errno));
}

FileDescriptor open_private_file(int directory, const std::string& name, int flags) {
    FileDescriptor file(::openat(directory, name.c_str(), flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    struct stat info{};
    if (file.get() < 0 || ::fstat(file.get(), &info) != 0) {
        throw make_error("unable to access " + name);
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 ||
        (info.st_mode & 07777) != 0600) {
        throw make_error(name + " must be a regular, user-owned 0600 file without additional hard links");
    }
    return file;
}

void sync_file(int fd) {
    while (::fsync(fd) != 0) {
        if (errno != EINTR) throw make_error("failed to flush identity storage");
    }
}

FileDescriptor acquire_lock(int directory, const std::string& name) {
    auto lock = open_private_file(directory, name, O_RDWR | O_CREAT);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
            throw make_error("unable to lock identity storage");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw make_error("timed out waiting for identity storage lock");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return lock;
}

size_t read_file(int directory, const std::string& name, unsigned char* data, size_t capacity) {
    auto file = open_private_file(directory, name, O_RDONLY);
    size_t size = 0;
    while (size < capacity) {
        const auto count = ::read(file.get(), data + size, capacity - size);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw make_error("failed to read " + name);
        }
        if (count == 0) return size;
        size += static_cast<size_t>(count);
    }
    unsigned char extra = 0;
    ssize_t count;
    do { count = ::read(file.get(), &extra, 1); } while (count < 0 && errno == EINTR);
    sodium_memzero(&extra, sizeof(extra));
    if (count != 0) throw make_error("invalid size or read failure in " + name);
    return size;
}

void atomic_write(int directory, const std::string& name, const unsigned char* data, size_t size) {
    const auto temporary = name + ".tmp";
    auto file = open_private_file(directory, temporary, O_WRONLY | O_CREAT | O_EXCL);
    size_t written = 0;
    while (written < size) {
        const auto count = ::write(file.get(), data + written, size - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw make_error("failed to write " + temporary);
        written += static_cast<size_t>(count);
    }
    sync_file(file.get());
    file.close_checked();
    if (::renameat(directory, temporary.c_str(), directory, name.c_str()) != 0) {
        throw make_error("failed to publish " + name);
    }
    sync_file(directory);
    // Failed writes deliberately leave evidence, never an invitation to rekey.
}

} // namespace

DeviceIdentity::DeviceIdentity(std::string device_id, const PublicKey& public_key, const PrivateKey& private_key)
    : device_id_(std::move(device_id)), public_key_(public_key), private_key_(private_key) {}

DeviceIdentity::~DeviceIdentity() {
    sodium_memzero(private_key_.data(), private_key_.size());
}

std::filesystem::path DeviceIdentity::default_identity_path() {
    return resolve_home_directory() / ".config" / "gasoline" / "device_id";
}

DeviceIdentity DeviceIdentity::load_or_create() {
    return load_or_create(default_identity_path());
}

DeviceIdentity DeviceIdentity::load_or_create(const std::filesystem::path& identity_path) {
    initialize_sodium();
    const auto path = std::filesystem::absolute(identity_path);
    const auto name = path.filename().string();
    if (name.empty() || name == "." || name == "..") throw make_error("invalid identity filename");
    const auto keys_name = name + ".keys";
    auto directory = open_identity_directory(path.parent_path());
    auto lock = acquire_lock(directory.get(), name + ".lock");
    if (entry_exists(directory.get(), name + ".tmp") || entry_exists(directory.get(), keys_name + ".tmp")) {
        throw make_error("interrupted identity initialization; restore the complete identity before retrying");
    }

    std::string uuid;
    bool keys_required = false;
    if (entry_exists(directory.get(), name)) {
        std::array<unsigned char, 64> contents{};
        const auto size = read_file(directory.get(), name, contents.data(), contents.size());
        std::string text(reinterpret_cast<const char*>(contents.data()), size);
        if (text.size() == UUID_SIZE + IDENTITY_MARKER.size() &&
            std::string_view(text).substr(UUID_SIZE) == IDENTITY_MARKER) {
            keys_required = true;
            text.resize(UUID_SIZE);
        } else {
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        }
        if (!is_valid_uuid(text)) throw make_error("invalid UUID or identity format");
        uuid = std::move(text);
    }

    const bool keys_exist = entry_exists(directory.get(), keys_name);
    if (keys_exist != keys_required) {
        throw make_error(keys_required ? "established key material is missing; identity will not be regenerated" :
                                       "key material has no matching versioned UUID identity");
    }

    PublicKey public_key{};
    SecretBuffer<crypto_sign_SECRETKEYBYTES> private_key;
    SecretBuffer<KEY_FILE_SIZE> record;
    if (keys_required) {
        if (read_file(directory.get(), keys_name, record.bytes.data(), record.bytes.size()) != KEY_FILE_SIZE ||
            std::memcmp(record.bytes.data(), KEY_MAGIC.data(), KEY_MAGIC.size()) != 0 ||
            std::memcmp(record.bytes.data() + KEY_MAGIC.size(), uuid.data(), UUID_SIZE) != 0) {
            throw make_error("invalid key record format, length, or UUID binding");
        }
        std::copy_n(record.bytes.data() + KEY_MAGIC.size() + UUID_SIZE, public_key.size(), public_key.data());
        std::copy_n(record.bytes.data() + KEY_MAGIC.size() + UUID_SIZE + public_key.size(),
                    private_key.bytes.size(), private_key.bytes.data());
        SecretBuffer<crypto_sign_SEEDBYTES> seed;
        SecretBuffer<crypto_sign_SECRETKEYBYTES> derived_private;
        PublicKey derived_public{};
        if (crypto_sign_ed25519_sk_to_seed(seed.bytes.data(), private_key.bytes.data()) != 0 ||
            crypto_sign_seed_keypair(derived_public.data(), derived_private.bytes.data(), seed.bytes.data()) != 0 ||
            sodium_memcmp(public_key.data(), derived_public.data(), public_key.size()) != 0 ||
            sodium_memcmp(private_key.bytes.data(), derived_private.bytes.data(), private_key.bytes.size()) != 0) {
            throw make_error("corrupt or inconsistent Ed25519 key material");
        }
    } else {
        if (uuid.empty()) uuid = generate_uuid();
        const auto manifest = uuid + std::string(IDENTITY_MARKER);
        // Commit the key-required marker before generating any key material.
        // A crash after this point must fail closed, never generate replacements.
        atomic_write(directory.get(), name, reinterpret_cast<const unsigned char*>(manifest.data()), manifest.size());
        if (crypto_sign_keypair(public_key.data(), private_key.bytes.data()) != 0) {
            throw make_error("Ed25519 key generation failed");
        }
        auto* output = record.bytes.data();
        output = std::copy(KEY_MAGIC.begin(), KEY_MAGIC.end(), output);
        output = std::copy(uuid.begin(), uuid.end(), output);
        output = std::copy(public_key.begin(), public_key.end(), output);
        std::copy(private_key.bytes.begin(), private_key.bytes.end(), output);
        atomic_write(directory.get(), keys_name, record.bytes.data(), record.bytes.size());
    }
    return DeviceIdentity(std::move(uuid), public_key, private_key.bytes);
}

const std::string& DeviceIdentity::device_id() const { return device_id_; }
const DeviceIdentity::PublicKey& DeviceIdentity::public_key() const { return public_key_; }
const DeviceIdentity::PrivateKey& DeviceIdentity::private_key() const { return private_key_; }

} // namespace gasoline
