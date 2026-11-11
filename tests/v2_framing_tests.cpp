#include "core/protocol/v2_framing.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace gasoline::protocol_v2;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Operation>
void rejects_with(Operation operation, FramingErrorCode expected, const std::string& message) {
    try {
        operation();
    } catch (const FramingError& error) {
        require(error.code() == expected, message + ": wrong error code");
        return;
    }
    throw std::runtime_error(message + ": operation succeeded");
}

std::string preface() {
    const auto bytes = preface_bytes();
    return {bytes.data(), bytes.size()};
}

void valid_preface_and_version() {
    const auto bytes = preface_bytes();
    require(bytes.size() == PREFACE_SIZE && bytes.substr(0, 8) == "GASOLINE" &&
            static_cast<unsigned char>(bytes[8]) == 0 &&
            static_cast<unsigned char>(bytes[9]) == WIRE_VERSION,
            "unexpected protocol v2 preface encoding");
    RecordParser parser;
    parser.consume(bytes.substr(0, 3), [](std::string_view) { return true; });
    require(!parser.preface_complete(), "fragmented preface completed early");
    parser.consume(bytes.substr(3), [](std::string_view) { return true; });
    require(parser.preface_complete(), "valid preface was not accepted");
    parser.finish();
}

void invalid_preface_and_version() {
    auto wrong_magic = preface();
    wrong_magic[0] = 'X';
    RecordParser magic_parser;
    rejects_with([&] { magic_parser.consume(wrong_magic, [](std::string_view) { return true; }); },
                 FramingErrorCode::InvalidPreface, "wrong magic accepted");

    auto wrong_version = preface();
    wrong_version.back() = 1;
    RecordParser version_parser;
    rejects_with([&] { version_parser.consume(wrong_version, [](std::string_view) { return true; }); },
                 FramingErrorCode::InvalidPreface, "wrong version accepted");
}

void minimum_and_maximum_records() {
    std::vector<size_t> lengths;
    RecordParser parser;
    std::string stream = preface() + encode_record("");
    const std::string maximum(MAX_RECORD_SIZE, 'm');
    stream += encode_record(maximum);
    parser.consume(stream, [&](std::string_view record) {
        lengths.push_back(record.size());
        if (record.size() == MAX_RECORD_SIZE) {
            require(record.front() == 'm' && record.back() == 'm', "maximum record was corrupted");
        }
        return true;
    });
    parser.finish();
    require(lengths == std::vector<size_t>({0, MAX_RECORD_SIZE}),
            "minimum or maximum record length was rejected");
    bool rejected = false;
    try {
        (void)encode_record(std::string(MAX_RECORD_SIZE + 1, 'x'));
    } catch (const std::length_error&) {
        rejected = true;
    }
    require(rejected, "oversized outgoing record was accepted");
}

void explicit_network_byte_order() {
    const std::string payload(258, 'e');
    const auto encoded = encode_record(payload);
    require(encoded.size() == RECORD_HEADER_SIZE + payload.size() &&
            static_cast<unsigned char>(encoded[0]) == 0x01 &&
            static_cast<unsigned char>(encoded[1]) == 0x02,
            "258-byte record was not encoded with a 01 02 network-order length");

    RecordParser parser;
    std::string wire = preface();
    wire.append("\x01\x02", 2);
    const std::string manually_framed_payload(258, 'p');
    wire += manually_framed_payload;
    size_t delivered = 0;
    parser.consume(wire, [&](std::string_view record) {
        require(record == manually_framed_payload,
                "manually framed 258-byte payload was corrupted");
        delivered = record.size();
        return true;
    });
    parser.finish();
    require(delivered == 258, "raw 01 02 header was not parsed as 258 bytes");
}

void oversized_before_payload_buffering() {
    RecordParser parser(8);
    parser.consume(preface(), [](std::string_view) { return true; });
    const std::string oversized_header{"\x00\x09", 2};
    rejects_with([&] { parser.consume(oversized_header, [](std::string_view) { return true; }); },
                 FramingErrorCode::OversizedRecord, "configured record limit was ignored");
    require(parser.buffered_payload_size() == 0,
            "oversized record buffered payload before rejection");
}

void split_header_and_payload() {
    RecordParser parser;
    std::vector<std::string> records;
    parser.consume(preface(), [&](std::string_view value) {
        records.emplace_back(value);
        return true;
    });
    const auto encoded = encode_record("fragmented");
    parser.consume(std::string_view(encoded).substr(0, 1), [&](std::string_view value) {
        records.emplace_back(value);
        return true;
    });
    require(records.empty(), "split header emitted a record");
    parser.consume(std::string_view(encoded).substr(1, 4), [&](std::string_view value) {
        records.emplace_back(value);
        return true;
    });
    require(records.empty(), "split payload emitted a record");
    parser.consume(std::string_view(encoded).substr(5), [&](std::string_view value) {
        records.emplace_back(value);
        return true;
    });
    parser.finish();
    require(records == std::vector<std::string>({"fragmented"}),
            "fragmented record was not reconstructed");
}

void multiple_and_back_to_back_records() {
    RecordParser parser;
    std::vector<std::string> records;
    const std::string stream = preface() + encode_record("one") + encode_record("two") +
                               encode_record("three");
    parser.consume(stream, [&](std::string_view value) {
        records.emplace_back(value);
        return true;
    });
    parser.finish();
    require(records == std::vector<std::string>({"one", "two", "three"}),
            "coalesced back-to-back records were parsed incorrectly");
}

void callback_stop_is_terminal() {
    RecordParser parser;
    size_t callbacks = 0;
    const bool completed = parser.consume(
        preface() + encode_record("first") + encode_record("must-not-run"),
        [&](std::string_view record) {
            ++callbacks;
            require(record == "first", "callback received the wrong first record");
            return false;
        });
    require(!completed && callbacks == 1, "callback stop did not halt coalesced input");
    require(parser.buffered_payload_size() == 0,
            "delivered payload remained reported as partially buffered");
    rejects_with([&] { parser.consume(encode_record("later"), [](std::string_view) { return true; }); },
                 FramingErrorCode::ParserFinished, "callback-stopped parser accepted more input");
    rejects_with([&] { parser.finish(); }, FramingErrorCode::ParserFinished,
                 "callback-stopped parser accepted EOF as resumable state");
}

void buffered_payload_reporting() {
    RecordParser parser;
    parser.consume(preface() + encode_record("delivered"), [](std::string_view) { return true; });
    require(parser.buffered_payload_size() == 0, "delivered record remained buffered");
    parser.consume(std::string("\x00\x05", 2) + "ab", [](std::string_view) { return true; });
    require(parser.buffered_payload_size() == 2, "partial payload size was reported incorrectly");
    parser.consume("cde", [](std::string_view record) { return record == "abcde"; });
    require(parser.buffered_payload_size() == 0, "completed partial payload remained buffered");
    parser.finish();
}

void truncated_streams() {
    RecordParser preface_parser;
    preface_parser.consume(preface().substr(0, 4), [](std::string_view) { return true; });
    rejects_with([&] { preface_parser.finish(); }, FramingErrorCode::TruncatedPreface,
                 "truncated preface accepted at EOF");

    RecordParser header_parser;
    header_parser.consume(preface() + std::string("\x00", 1), [](std::string_view) { return true; });
    rejects_with([&] { header_parser.finish(); }, FramingErrorCode::TruncatedHeader,
                 "truncated header accepted at EOF");

    RecordParser payload_parser;
    payload_parser.consume(preface() + std::string("\x00\x05", 2) + "abc",
                           [](std::string_view) { return true; });
    rejects_with([&] { payload_parser.finish(); }, FramingErrorCode::TruncatedPayload,
                 "truncated payload accepted at EOF");
}

void malformed_and_plaintext_rejection() {
    RecordParser garbled;
    auto bytes = preface();
    bytes[5] ^= 0x20;
    rejects_with([&] { garbled.consume(bytes, [](std::string_view) { return true; }); },
                 FramingErrorCode::InvalidPreface, "garbled preface accepted");

    RecordParser plaintext;
    rejects_with([&] {
        plaintext.consume("{\"type\":\"hello\"}\n", [](std::string_view) { return true; });
    }, FramingErrorCode::InvalidPreface, "newline-delimited v1 plaintext fallback was accepted");
}

void clean_eof() {
    RecordParser parser;
    bool received = false;
    parser.consume(preface() + encode_record("complete"), [&](std::string_view record) {
        received = record == "complete";
        return true;
    });
    parser.finish();
    parser.finish();
    require(received, "complete record missing before clean EOF");
}

} // namespace

int main() {
    try {
        const std::pair<const char*, void(*)()> tests[] = {
            {"valid fragmented v2 preface and version", valid_preface_and_version},
            {"invalid preface and unsupported version", invalid_preface_and_version},
            {"zero and 65,535-byte records", minimum_and_maximum_records},
            {"explicit 258-byte network-order vectors", explicit_network_byte_order},
            {"oversized record rejected before payload buffering", oversized_before_payload_buffering},
            {"split record header and payload", split_header_and_payload},
            {"multiple back-to-back records in one input", multiple_and_back_to_back_records},
            {"callback stop terminally discards coalesced suffix", callback_stop_is_terminal},
            {"only incomplete payload bytes are reported buffered", buffered_payload_reporting},
            {"truncated preface, header, and payload", truncated_streams},
            {"garbled input and no plaintext fallback", malformed_and_plaintext_rejection},
            {"clean EOF at a record boundary", clean_eof},
        };
        for (const auto& test : tests) {
            test.second();
            std::cout << "PASS: " << test.first << std::endl;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
