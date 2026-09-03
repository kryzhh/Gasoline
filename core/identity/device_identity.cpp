#include "device_identity.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <thread>

namespace gasoline {

namespace {

std::runtime_error make_error(const std::string& message) {
    return std::runtime_error("Device identity error: " + message);
}

std::filesystem::path resolve_home_directory() {
    struct passwd* password_entry = getpwuid(getuid());
    if (password_entry == nullptr || password_entry->pw_dir == nullptr || password_entry->pw_dir[0] == '\0') {
        throw make_error("unable to determine the current user's home directory");
    }

    return std::filesystem::path(password_entry->pw_dir);
}

bool is_hex_digit(char value) {
    return std::isdigit(static_cast<unsigned char>(value)) != 0 ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool is_valid_uuid(const std::string& value) {
    if (value.size() != 36) {
        return false;
    }

    for (size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') {
                return false;
            }
            continue;
        }

        if (!is_hex_digit(value[index])) {
            return false;
        }
    }

    return true;
}

std::string read_file_contents(const std::filesystem::path& identity_path) {
    std::ifstream file(identity_path);
    if (!file.is_open()) {
        throw make_error("failed to open identity file: " + identity_path.string());
    }

    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) {
        throw make_error("failed to read identity file: " + identity_path.string());
    }

    while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r')) {
        contents.pop_back();
    }

    if (!is_valid_uuid(contents)) {
        throw make_error("identity file contains an invalid device ID: " + identity_path.string());
    }

    return contents;
}

std::string generate_uuid() {
    std::array<unsigned char, 16> bytes{};

    const int random_fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0) {
        throw make_error(std::string("failed to open /dev/urandom: ") + std::strerror(errno));
    }

    size_t bytes_read = 0;
    while (bytes_read < bytes.size()) {
        const ssize_t count = ::read(random_fd, bytes.data() + bytes_read, bytes.size() - bytes_read);
        if (count < 0) {
            const int error_code = errno;
            ::close(random_fd);
            throw make_error(std::string("failed to read random bytes: ") + std::strerror(error_code));
        }
        if (count == 0) {
            ::close(random_fd);
            throw make_error("unexpected end of random source while generating identity");
        }

        bytes_read += static_cast<size_t>(count);
    }

    ::close(random_fd);

    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);

    static const char* hex_digits = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);

    for (size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            uuid.push_back('-');
        }
        uuid.push_back(hex_digits[(bytes[index] >> 4) & 0x0F]);
        uuid.push_back(hex_digits[bytes[index] & 0x0F]);
    }

    return uuid;
}

void write_file_contents(const std::filesystem::path& file_path, const std::string& contents) {
    const int file_fd = ::open(file_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (file_fd < 0) {
        throw make_error(std::string("failed to create identity file: ") + file_path.string() + ": " + std::strerror(errno));
    }

    const std::string data = contents + "\n";
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = ::write(file_fd, data.data() + written, data.size() - written);
        if (count < 0) {
            const int error_code = errno;
            ::close(file_fd);
            throw make_error(std::string("failed to write identity file: ") + file_path.string() + ": " + std::strerror(error_code));
        }
        written += static_cast<size_t>(count);
    }

    if (::fsync(file_fd) != 0) {
        const int error_code = errno;
        ::close(file_fd);
        throw make_error(std::string("failed to flush identity file: ") + file_path.string() + ": " + std::strerror(error_code));
    }

    if (::close(file_fd) != 0) {
        throw make_error(std::string("failed to close identity file: ") + file_path.string() + ": " + std::strerror(errno));
    }
}

void remove_if_exists(const std::filesystem::path& file_path) {
    std::error_code error;
    std::filesystem::remove(file_path, error);
}

std::filesystem::path lock_path_for(const std::filesystem::path& identity_path) {
    return identity_path.string() + ".lock";
}

std::filesystem::path temp_path_for(const std::filesystem::path& identity_path) {
    return identity_path.string() + ".tmp";
}

void ensure_parent_directory(const std::filesystem::path& identity_path) {
    const std::filesystem::path parent = identity_path.parent_path();
    if (parent.empty()) {
        return;
    }

    std::error_code error;
    if (std::filesystem::exists(parent, error) && !std::filesystem::is_directory(parent, error)) {
        throw make_error("identity path parent exists but is not a directory: " + parent.string());
    }

    if (!std::filesystem::create_directories(parent, error) && error) {
        throw make_error("failed to create identity directory: " + parent.string() + ": " + error.message());
    }
}

} // namespace

DeviceIdentity::DeviceIdentity(std::string device_id)
    : device_id_(std::move(device_id)) {}

std::filesystem::path DeviceIdentity::default_identity_path() {
    return resolve_home_directory() / ".config" / "gasoline" / "device_id";
}

DeviceIdentity DeviceIdentity::load_or_create() {
    return load_or_create(default_identity_path());
}

DeviceIdentity DeviceIdentity::load_or_create(const std::filesystem::path& identity_path) {
    std::error_code exists_error;
    if (std::filesystem::exists(identity_path, exists_error)) {
        return DeviceIdentity(read_file_contents(identity_path));
    }
    if (exists_error) {
        throw make_error("failed to inspect identity file: " + identity_path.string() + ": " + exists_error.message());
    }

    ensure_parent_directory(identity_path);

    const std::filesystem::path lock_path = lock_path_for(identity_path);
    const std::filesystem::path temp_path = temp_path_for(identity_path);

    int lock_fd = -1;
    for (int attempt = 0; attempt < 200; ++attempt) {
        lock_fd = ::open(lock_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (lock_fd >= 0) {
            break;
        }

        if (errno != EEXIST) {
            throw make_error(std::string("failed to acquire identity lock: ") + lock_path.string() + ": " + std::strerror(errno));
        }

        if (std::filesystem::exists(identity_path)) {
            return DeviceIdentity(read_file_contents(identity_path));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (lock_fd < 0) {
        if (std::filesystem::exists(identity_path)) {
            return DeviceIdentity(read_file_contents(identity_path));
        }

        throw make_error("timed out waiting for another process to initialize the identity file: " + identity_path.string());
    }

    auto cleanup = [&]() {
        if (lock_fd >= 0) {
            ::close(lock_fd);
            lock_fd = -1;
        }
        remove_if_exists(lock_path);
        remove_if_exists(temp_path);
    };

    try {
        if (std::filesystem::exists(identity_path)) {
            cleanup();
            return DeviceIdentity(read_file_contents(identity_path));
        }

        const std::string new_identity = generate_uuid();
        write_file_contents(temp_path, new_identity);

        std::error_code rename_error;
        std::filesystem::rename(temp_path, identity_path, rename_error);
        if (rename_error) {
            cleanup();
            if (std::filesystem::exists(identity_path)) {
                return DeviceIdentity(read_file_contents(identity_path));
            }
            throw make_error("failed to persist identity file: " + identity_path.string() + ": " + rename_error.message());
        }

        cleanup();
        return DeviceIdentity(new_identity);
    } catch (...) {
        cleanup();
        throw;
    }
}

const std::string& DeviceIdentity::device_id() const {
    return device_id_;
}

} // namespace gasoline