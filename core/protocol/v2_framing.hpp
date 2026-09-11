#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gasoline::protocol_v2 {

inline constexpr uint16_t WIRE_VERSION = 2;
inline constexpr size_t PREFACE_SIZE = 10;
inline constexpr size_t RECORD_HEADER_SIZE = 2;
inline constexpr size_t MAX_RECORD_SIZE = 65535;

std::string_view preface_bytes();
std::string encode_record(std::string_view payload);

enum class FramingErrorCode {
    InvalidPreface,
    OversizedRecord,
    TruncatedPreface,
    TruncatedHeader,
    TruncatedPayload,
    ParserFinished
};

class FramingError : public std::runtime_error {
public:
    FramingError(FramingErrorCode code, const std::string& message);
    FramingErrorCode code() const noexcept;

private:
    FramingErrorCode code_;
};

class RecordParser {
public:
    using RecordHandler = std::function<bool(std::string_view)>;

    explicit RecordParser(size_t maximum_record_size = MAX_RECORD_SIZE);

    // Consumes arbitrary stream fragments. The callback is invoked synchronously
    // once per complete record. Returning false terminally stops this parser:
    // unread bytes from this input are discarded and later consume/finish calls
    // throw ParserFinished. It is not a resumable backpressure mechanism.
    bool consume(std::string_view input, const RecordHandler& handler);

    // Marks EOF. EOF is clean only after a complete preface at a record boundary.
    void finish();

    bool preface_complete() const noexcept;
    // Bytes held for the current incomplete payload; delivered records are clear.
    size_t buffered_payload_size() const noexcept;

private:
    enum class Phase {
        Preface,
        Header,
        Payload
    };

    [[noreturn]] void fail(FramingErrorCode code, const char* message);

    const size_t maximum_record_size_;
    Phase phase_{Phase::Preface};
    size_t preface_offset_{0};
    uint8_t header_[RECORD_HEADER_SIZE]{};
    size_t header_offset_{0};
    size_t expected_payload_size_{0};
    std::string payload_;
    bool failed_{false};
    bool finished_{false};
};

} // namespace gasoline::protocol_v2
