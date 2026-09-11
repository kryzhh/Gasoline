#include "v2_framing.hpp"

#include <algorithm>
#include <array>

namespace gasoline::protocol_v2 {
namespace {

constexpr std::array<char, PREFACE_SIZE> PREFACE{
    'G', 'A', 'S', 'O', 'L', 'I', 'N', 'E',
    static_cast<char>((WIRE_VERSION >> 8) & 0xff),
    static_cast<char>(WIRE_VERSION & 0xff)
};

} // namespace

std::string_view preface_bytes() {
    return {PREFACE.data(), PREFACE.size()};
}

std::string encode_record(std::string_view payload) {
    if (payload.size() > MAX_RECORD_SIZE) {
        throw std::length_error("protocol v2 record exceeds 16-bit length limit");
    }
    std::string encoded;
    encoded.reserve(RECORD_HEADER_SIZE + payload.size());
    encoded.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
    encoded.push_back(static_cast<char>(payload.size() & 0xff));
    encoded.append(payload.data(), payload.size());
    return encoded;
}

FramingError::FramingError(FramingErrorCode code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

FramingErrorCode FramingError::code() const noexcept {
    return code_;
}

RecordParser::RecordParser(size_t maximum_record_size)
    : maximum_record_size_(maximum_record_size) {
    if (maximum_record_size > MAX_RECORD_SIZE) {
        throw std::invalid_argument("record parser limit exceeds protocol v2 maximum");
    }
}

[[noreturn]] void RecordParser::fail(FramingErrorCode code, const char* message) {
    failed_ = true;
    throw FramingError(code, message);
}

bool RecordParser::consume(std::string_view input, const RecordHandler& handler) {
    if (failed_ || finished_) {
        fail(FramingErrorCode::ParserFinished, "protocol v2 parser is not accepting input");
    }

    while (!input.empty()) {
        if (phase_ == Phase::Preface) {
            const auto expected = preface_bytes();
            while (!input.empty() && preface_offset_ < expected.size()) {
                if (input.front() != expected[preface_offset_]) {
                    fail(FramingErrorCode::InvalidPreface, "invalid protocol v2 preface or version");
                }
                ++preface_offset_;
                input.remove_prefix(1);
            }
            if (preface_offset_ == expected.size()) {
                phase_ = Phase::Header;
            }
            continue;
        }

        if (phase_ == Phase::Header) {
            while (!input.empty() && header_offset_ < RECORD_HEADER_SIZE) {
                header_[header_offset_++] = static_cast<uint8_t>(input.front());
                input.remove_prefix(1);
            }
            if (header_offset_ != RECORD_HEADER_SIZE) {
                continue;
            }

            expected_payload_size_ = (static_cast<size_t>(header_[0]) << 8) |
                                     static_cast<size_t>(header_[1]);
            header_offset_ = 0;
            if (expected_payload_size_ > maximum_record_size_) {
                fail(FramingErrorCode::OversizedRecord, "protocol v2 record exceeds configured limit");
            }
            if (expected_payload_size_ == 0) {
                payload_.clear();
                if (!handler(std::string_view{})) {
                    failed_ = true;
                    return false;
                }
                continue;
            }

            // The length is validated before the only attacker-length-driven
            // allocation. Capacity is retained and remains bounded by the limit.
            payload_.clear();
            payload_.reserve(expected_payload_size_);
            phase_ = Phase::Payload;
            continue;
        }

        const size_t remaining = expected_payload_size_ - payload_.size();
        const size_t count = std::min(remaining, input.size());
        payload_.append(input.data(), count);
        input.remove_prefix(count);
        if (payload_.size() == expected_payload_size_) {
            phase_ = Phase::Header;
            expected_payload_size_ = 0;
            const bool keep_processing = handler(payload_);
            payload_.clear();
            if (!keep_processing) {
                failed_ = true;
                return false;
            }
        }
    }
    return true;
}

void RecordParser::finish() {
    if (failed_) {
        fail(FramingErrorCode::ParserFinished, "protocol v2 parser already failed");
    }
    if (finished_) {
        return;
    }
    if (phase_ == Phase::Preface) {
        fail(FramingErrorCode::TruncatedPreface, "truncated protocol v2 preface");
    }
    if (phase_ == Phase::Payload) {
        fail(FramingErrorCode::TruncatedPayload, "truncated protocol v2 record payload");
    }
    if (header_offset_ != 0) {
        fail(FramingErrorCode::TruncatedHeader, "truncated protocol v2 record header");
    }
    finished_ = true;
}

bool RecordParser::preface_complete() const noexcept {
    return phase_ != Phase::Preface;
}

size_t RecordParser::buffered_payload_size() const noexcept {
    return payload_.size();
}

} // namespace gasoline::protocol_v2
