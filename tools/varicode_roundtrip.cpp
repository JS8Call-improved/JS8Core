#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "js8core/protocol/varicode.hpp"

int main() {
  int failures = 0;
  auto check = [&](bool condition, std::string const& description) {
    if (!condition) {
      std::cerr << "FAIL: " << description << "\n";
      ++failures;
      return;
    }
    std::cout << "OK: " << description << "\n";
  };

  {
    // Fixed wire octets, independently specified as direct command IDs
    // (0x00..0x7f) and two numeric SNR command regions (0x80..0xff).
    using js8core::protocol::varicode::pack_cmd;
    using js8core::protocol::varicode::unpack_cmd;
    struct WireCase {
      std::uint8_t command;
      std::uint8_t number;
      std::uint8_t octet;
      bool has_number;
    };
    constexpr std::array<WireCase, 9> cases{{
        {0, 0, 0x00, false}, {24, 63, 0x18, false},
        {31, 99, 0x1f, false}, {25, 0, 0x80, true},
        {25, 31, 0x9f, true}, {25, 63, 0xbf, true},
        {29, 0, 0xc0, true}, {29, 31, 0xdf, true},
        {29, 63, 0xff, true},
    }};
    bool vectors_match = true;
    for (auto const& item : cases) {
      bool packed_number = false;
      auto const octet = pack_cmd(item.command, item.number, &packed_number);
      std::uint8_t payload = 255;
      auto const decoded = unpack_cmd(item.octet, &payload);
      vectors_match = vectors_match && octet == item.octet &&
                      packed_number == item.has_number &&
                      decoded == item.command &&
                      payload == (item.has_number ? item.number : 0);
    }
    check(vectors_match, "command-byte known-answer vectors match JS8 wire format");

    bool direct_space_matches = true;
    for (unsigned cmd = 0; cmd < 128; ++cmd) {
      if (cmd == 25 || cmd == 29) continue;
      bool packed_number = true;
      std::uint8_t returned_number = 255;
      const auto wire = pack_cmd(static_cast<std::uint8_t>(cmd), 111,
                                 &packed_number);
      const auto decoded = unpack_cmd(wire, &returned_number);
      direct_space_matches = direct_space_matches &&
                             wire == cmd && decoded == cmd &&
                             !packed_number && returned_number == 0;
    }
    check(direct_space_matches, "every direct command ID preserves its octet");

    bool numeric_space_matches = true;
    for (auto const cmd : {25u, 29u}) {
      for (unsigned number = 0; number < 256; ++number) {
        bool carries_number = false;
        auto const wire = pack_cmd(static_cast<std::uint8_t>(cmd),
                                   static_cast<std::uint8_t>(number),
                                   &carries_number);
        std::uint8_t decoded_number = 255;
        const auto decoded = unpack_cmd(wire, &decoded_number);
        const unsigned expected = (cmd == 25 ? 0x80 : 0xc0) + number % 64;
        numeric_space_matches = numeric_space_matches &&
                                carries_number && wire == expected &&
                                decoded == cmd && decoded_number == number % 64;
      }
    }
    check(numeric_space_matches, "numeric command regions and six-bit wrapping");

    check(pack_cmd(25, 255, nullptr) == 0xbf &&
          pack_cmd(29, 64, nullptr) == 0xc0 &&
          unpack_cmd(0xff, nullptr) == 29,
          "command-byte APIs permit null output pointers");
  }

  {
    // The original public helper consumes high-byte escapes without output.
    const std::string utf8 = std::string("Hello ") +
                             static_cast<char>(0xc3) + static_cast<char>(0xa9) +
                             " " + static_cast<char>(0xf0) +
                             static_cast<char>(0x9f) + static_cast<char>(0x8c) +
                             static_cast<char>(0x8d);
    using namespace js8core::protocol::varicode;
    check(unescape(escape(utf8)) == "Hello  ",
          "legacy unescape drops escaped high-byte values");
    check(unescape(R"(A\u0042\U007fC)") == std::string("AB\x7f" "C", 4),
          "ASCII escapes still produce decoded characters");
  }

  {
    // Valid high-codepoint escapes are consumed; malformed ones stay literal.
    check(js8core::protocol::varicode::unescape("\\u263A") == "",
          "legacy non-ASCII Unicode escape is consumed without output");
    check(js8core::protocol::varicode::unescape("\\u12x4") == "\\u12x4",
          "malformed escapes remain literal");

    // An unsupported payload must be rejected promptly rather than stall
    // build_message_frames() indefinitely with the same unconsumed input.
    std::string const invalid(1, static_cast<char>(0xff));
    auto frames = js8core::protocol::varicode::build_message_frames(
        "K1ABC", "EM73", "", invalid, false, true, 0, nullptr);
    check(frames.empty(), "unencodable forced data fails without a loop");
  }

  std::vector<std::string> samples = {
      "CQ CQ CQ",
      "HB EM73",
      "KN4CRD: HELLO WORLD",
      "`@ALLCALL HB",
      "J1Y ACK 73",
      "TEST FASTDATA PAYLOAD"
  };

  for (auto const& s : samples) {
    std::uint8_t t = 0;
    std::uint16_t num = 0;
    std::uint8_t bits3 = 0;
    int n = 0;
    auto hb = js8core::protocol::varicode::pack_heartbeat_message(s, "KN4CRD", &n);
    if (!hb.empty()) {
      auto unpacked = js8core::protocol::varicode::unpack_heartbeat_message(hb, &t, nullptr, &bits3);
      check(!unpacked.empty(), "heartbeat roundtrip for '" + s + "'");
    }
    auto cmp = js8core::protocol::varicode::pack_compound_message("`" + s, &n);
    if (!cmp.empty()) {
      auto unpacked = js8core::protocol::varicode::unpack_compound_message(cmp, &t, &num, &bits3);
      check(!unpacked.empty(), "compound roundtrip for '" + s + "'");
    }
    auto fast = js8core::protocol::varicode::pack_fast_data_message(s, &n);
    auto fast_dec = js8core::protocol::varicode::unpack_fast_data_message(fast);
    check(fast_dec == s, "fast-data roundtrip for '" + s + "'");
  }

  {
    // Regression: forced data must not be modified by automatic identification.
    auto frames = js8core::protocol::varicode::build_message_frames(
        "KN4CRD", "EM73", "", "PAYLOAD", true, true, 2, nullptr);
    check(frames.size() == 1, "forced Turbo data produces one frame");
    if (!frames.empty()) {
      check(js8core::protocol::varicode::unpack_fast_data_message(frames.front().first) == "PAYLOAD",
            "forced Turbo data preserves payload without sender callsign");
    }
  }

  {
    check(!js8core::protocol::varicode::is_valid_callsign("HELLO", nullptr),
          "ordinary text is not accepted as a callsign");
    check(js8core::protocol::varicode::is_valid_callsign("2E0ABC", nullptr),
          "callsign beginning with a digit is accepted");
    check(js8core::protocol::varicode::is_valid_callsign("EA8/K1ABC", nullptr),
          "compound callsign is accepted");

    bool is_compound = false;
    check(js8core::protocol::varicode::is_valid_callsign("K1AAAAA", &is_compound) &&
              is_compound,
          "seven-character call has compound identity, not normal identity");
    is_compound = true;
    check(js8core::protocol::varicode::is_valid_callsign("K1ABC/P", &is_compound) &&
              !is_compound,
          "portable short call remains normal");
    check(!js8core::protocol::varicode::is_valid_callsign("K1@ABC", nullptr) &&
          !js8core::protocol::varicode::is_valid_callsign("K1AB@C", nullptr),
          "compound callsign grammar rejects embedded at-signs");
    check(js8core::protocol::varicode::is_valid_callsign("K1AAAAAA", &is_compound) &&
          is_compound,
          "eight-character call remains a valid compound identity");

    auto frames = js8core::protocol::varicode::build_message_frames(
        "W1ABC", "EM73", "", "JUST TESTING", true, false, 0, nullptr);
    check(!frames.empty(), "forced identification produces frames for ordinary text");
    bool identified = false;
    for (auto const& frame : frames) {
      auto text = js8core::protocol::varicode::unpack_data_message(frame.first);
      identified = identified || text.find("W1ABC:") != std::string::npos;
    }
    check(identified, "forced identification prefixes sender to ordinary text");
  }

  {
    std::string to, cmd, num;
    bool compound = false;
    int consumed = 0;
    auto directed = js8core::protocol::varicode::pack_directed_message(
        "2E0ABC HELLO", "VE7NHW", &to, &compound, &cmd, &num, &consumed);
    check(!directed.empty() && to == "2E0ABC", "relay keeps leading digit in destination callsign");

    auto frames = js8core::protocol::varicode::build_message_frames(
        "VE7NHW", "CN89", "", "N0XYZ RR 73", false, false, 0, nullptr);
    bool saw_rr = false;
    bool saw_remainder = false;
    for (auto const& frame : frames) {
      auto decoded = js8core::protocol::varicode::unpack_directed_message(frame.first, nullptr);
      if (decoded.size() >= 3 && decoded[2] == " RR") {
        saw_rr = true;
      }
      saw_remainder = saw_remainder || js8core::protocol::varicode::unpack_data_message(frame.first) == "73";
    }
    check(saw_rr, "RR 73 is not parsed as an SNR number");
    check(saw_remainder, "non-buffered command remainder is left-stripped");

    // Desktop treats space-bearing recognized commands as buffered even
    // when their numeric command ID is not in the explicit buffering group.
    // The following payload must therefore survive directed-frame packing.
    auto fb_frames = js8core::protocol::varicode::build_message_frames(
        "VE7NHW", "CN89", "", "N0XYZ FB HELLO", false, false, 0, nullptr);
    bool saw_fb_command = false;
    bool saw_fb_text = false;
    for (auto const& part : fb_frames) {
      auto const fields =
          js8core::protocol::varicode::unpack_directed_message(part.first, nullptr);
      saw_fb_command = saw_fb_command ||
                       (fields.size() >= 3 && fields[2] == " FB");
      auto const data = js8core::protocol::varicode::unpack_data_message(part.first);
      saw_fb_text = saw_fb_text || data == "HELLO";
    }
    check(saw_fb_command && saw_fb_text,
          "space-bearing FB command preserves its following data frame");


    auto snr_frame = js8core::protocol::varicode::pack_directed_message(
        "K1ABC SNR -8", "VE7NHW", &to, &compound, &cmd, &num, &consumed);
    auto snr_decoded = js8core::protocol::varicode::unpack_directed_message(snr_frame, nullptr);
    check(snr_decoded.size() >= 4 && snr_decoded[3] == "-08", "SNR is zero-padded like desktop");
  }

  {
    int chars = 0;
    auto frame = js8core::protocol::varicode::pack_data_message("N0XYZ QUERY CALL W1ABC?", &chars);
    check(!frame.empty() && chars > 0, "normal-speed data selects a usable compressed frame");
    auto const decoded_data = js8core::protocol::varicode::unpack_data_message(frame);
    check(decoded_data == std::string("N0XYZ QUERY CALL W1ABC?").substr(0, decoded_data.size()) &&
              decoded_data.size() == static_cast<std::size_t>(chars),
          "normal-speed compressed frame decodes its packed text prefix");
  }

  {
    auto frames = js8core::protocol::varicode::build_message_frames(
        "VE7NHW", "CN89", "", "@APRSIS MSG HELLO APRS", false, false, 0, nullptr);
    bool has_message_data = false;
    for (auto const& frame : frames) {
      auto payload = js8core::protocol::varicode::unpack_data_message(frame.first);
      has_message_data = has_message_data || payload == "HELLO APRS";
    }
    check(has_message_data, "@APRSIS MSG data is sent without an appended checksum");
  }

  {
    // Regression: an explicit destination must not be replaced by selectedCall.
    auto frames = js8core::protocol::varicode::build_message_frames(
        "KN4CRD", "EM73", "W1AW", "K1ABC SNR?", false, false, 2, nullptr);
    bool addressedToExplicitCall = false;
    for (auto const& frame : frames) {
      std::uint8_t type = 0;
      auto decoded = js8core::protocol::varicode::unpack_directed_message(frame.first, &type);
      if (decoded.size() > 1 && decoded[1] == "K1ABC") addressedToExplicitCall = true;
    }
    check(addressedToExplicitCall, "explicit Turbo destination is not auto-prefixed");
  }

  {
    // Regression: command-only compound helper frame must not decode as a fake grid (e.g. RA90).
    std::uint8_t type = 0;
    std::uint16_t extra = 0;
    std::uint8_t bits3 = 0;
    int n = 0;

    auto helper = js8core::protocol::varicode::pack_compound_message("`MO1QF ", &n);
    check(!helper.empty(), "pack compound helper '`MO1QF '");

    auto unpacked = js8core::protocol::varicode::unpack_compound_message(helper, &type, &extra, &bits3);
    check(!unpacked.empty(), "unpack compound helper '`MO1QF '");
    check(extra > 180 * 180, "compound helper extra uses command range");

    std::string rendered;
    for (auto part : unpacked) {
      while (!part.empty() && part.front() == ' ') part.erase(part.begin());
      if (part.empty()) continue;
      if (!rendered.empty()) rendered += " ";
      rendered += part;
    }
    check(rendered.find("RA90") == std::string::npos, "compound helper does not render RA90");
  }

  {
    check(!js8core::protocol::varicode::is_compound_callsign("K1ABC/P"),
          "portable /P callsign is not compound");
    check(js8core::protocol::varicode::is_compound_callsign("EA8/K1ABC"),
          "prefix compound callsign remains compound");

    auto frames = js8core::protocol::varicode::build_message_frames(
        "K1ABC/P", "EM73", "W1AW", "W1AW MSG TEST", false, false, 0, nullptr);
    bool saw_directed = false;
    bool saw_compound_directed = false;
    for (auto const& frame : frames) {
      std::uint8_t directed_type = 0;
      saw_directed = saw_directed || !js8core::protocol::varicode::unpack_directed_message(frame.first, &directed_type).empty();
      std::uint8_t compound_type = 0;
      saw_compound_directed = saw_compound_directed ||
          (!js8core::protocol::varicode::unpack_compound_message(frame.first, &compound_type, nullptr, nullptr).empty() && compound_type == 2);
    }
    check(saw_directed, "portable sender uses a standard directed frame");
    check(!saw_compound_directed, "portable sender does not use a compound-directed frame");
  }

  {
    // Regression: compound sender should use a compound-directed frame (no <....> over the air).
    auto frames = js8core::protocol::varicode::build_message_frames(
        "2W0OXE/5", "IO81", "MO1QF", "MO1QF MSG TEST", false, false, 0, nullptr);
    check(frames.size() >= 2, "compound sender directed free-text produces multiple frames");

    bool saw_ra90 = false;
    bool saw_directed = false;
    bool saw_compound_directed = false;
    for (auto const& frame : frames) {
      std::uint8_t directed_type = 0;
      auto directed = js8core::protocol::varicode::unpack_directed_message(frame.first, &directed_type);
      if (!directed.empty()) {
        saw_directed = true;
      }

      std::uint8_t type = 0;
      std::uint16_t extra = 0;
      std::uint8_t bits3 = 0;
      auto unpacked = js8core::protocol::varicode::unpack_compound_message(frame.first, &type, &extra, &bits3);
      if (unpacked.empty()) continue;
      if (type == 2) {
        saw_compound_directed = true;
      }
      std::string rendered;
      for (auto part : unpacked) {
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        if (part.empty()) continue;
        if (!rendered.empty()) rendered += " ";
        rendered += part;
      }
      if (rendered.find("RA90") != std::string::npos) {
        saw_ra90 = true;
      }
    }
    check(saw_compound_directed, "compound sender to normal recipient uses compound-directed frame");
    check(!saw_directed, "compound sender to normal recipient does not emit standard directed frame");
    check(!saw_ra90, "compound sender directed decode has no RA90 artifact");
  }

  {
    // Regression: compound recipient should use a compound-directed frame.
    auto frames = js8core::protocol::varicode::build_message_frames(
        "2W0OXE", "IO81", "MO1QF/5", "MO1QF/5 MSG TEST", false, false, 0, nullptr);
    check(frames.size() >= 2, "compound recipient directed free-text produces multiple frames");

    bool saw_directed = false;
    bool saw_compound_directed = false;
    for (auto const& frame : frames) {
      std::uint8_t directed_type = 0;
      auto directed = js8core::protocol::varicode::unpack_directed_message(frame.first, &directed_type);
      if (!directed.empty()) {
        saw_directed = true;
      }

      std::uint8_t type = 0;
      std::uint16_t extra = 0;
      std::uint8_t bits3 = 0;
      auto unpacked = js8core::protocol::varicode::unpack_compound_message(frame.first, &type, &extra, &bits3);
      if (!unpacked.empty() && type == 2) {
        saw_compound_directed = true;
      }
    }
    check(saw_compound_directed, "compound recipient uses compound-directed frame");
    check(!saw_directed, "compound recipient does not emit standard directed frame");
  }

  {
    // Regression: compound sender and recipient should use a compound-directed frame.
    auto frames = js8core::protocol::varicode::build_message_frames(
        "2W0OXE/5", "IO81", "MO1QF/5", "MO1QF/5 MSG TEST", false, false, 0, nullptr);
    check(frames.size() >= 2, "compound sender and recipient produce multiple frames");

    bool saw_directed = false;
    bool saw_compound_directed = false;
    for (auto const& frame : frames) {
      std::uint8_t directed_type = 0;
      auto directed = js8core::protocol::varicode::unpack_directed_message(frame.first, &directed_type);
      if (!directed.empty()) {
        saw_directed = true;
      }

      std::uint8_t type = 0;
      std::uint16_t extra = 0;
      std::uint8_t bits3 = 0;
      auto unpacked = js8core::protocol::varicode::unpack_compound_message(frame.first, &type, &extra, &bits3);
      if (!unpacked.empty() && type == 2) {
        saw_compound_directed = true;
      }
    }
    check(saw_compound_directed, "compound sender and recipient use compound-directed frame");
    check(!saw_directed, "compound sender and recipient do not emit standard directed frame");
  }

  {
    // Reference JS8 wire payloads are protocol test data. These assertions
    // independently verify both directions of the compound command codec.
    struct OutboundReference {
      const char* sender;
      const char* text;
      const char* first;
      const char* second;
    };
    constexpr std::array<OutboundReference, 9> transmit_references{{
        {"VE7NHW", "KG4UHM/6 SNR -03", "Bu7hI9zNOrMu", "IXXfezaRO+cm"},
        {"VE7NHW", "KG4UHM/6 SNR +05", "Bu7hI9zNOrMu", "IXXfezaRO+dm"},
        {"VE7NHW", "KG4UHM/6 HEARTBEAT SNR -08", "Bu7hI9zNOrMu", "IXXfezaRO+k8"},
        {"VE7NHW", "KG4UHM/6 HW CPY?", "Bu7hI9zNOrMu", "IXXfezaRO+Le"},
        {"VE7NHW", "KG4UHM/6 RR", "Bu7hI9zNOrMu", "IXXfezaRO+Lu"},
        {"VE7NHW", "KG4UHM/6 SNR?", "Bu7hI9zNOrMu", "IXXfezaRO+JG"},
        {"KG4UHM/6", "KJ7PRS SNR -03", "AXXfezaROrMu", "IYAexYQ7O+cm"},
        {"KG4UHM/6", "KJ7PRS HEARTBEAT SNR -08", "AXXfezaROrMu", "IYAexYQ7O+k8"},
        {"KG4UHM/6", "VE7NHW FB", "AXXfezaROrMu", "Ju7hI9zNO+LW"},
    }};
    std::size_t tx_passes = 0;
    for (const auto& reference : transmit_references) {
      const auto frames = js8core::protocol::varicode::build_message_frames(
          reference.sender, "CN89", "", reference.text,
          false, false, 0, nullptr);
      if (frames.size() != 2) continue;
      const bool exact_payloads =
          frames[0].first == reference.first &&
          frames[1].first == reference.second;
      const bool frame_kinds = frames[0].second == 1 && frames[1].second == 2;
      if (exact_payloads && frame_kinds) ++tx_passes;
    }
    check(tx_passes == transmit_references.size(),
          "all 9 compound-command transmit wire references");

    struct InboundReference {
      const char* symbols;
      const char* origin;
      const char* operation;
      const char* numeric_value;
    };
    constexpr std::array<InboundReference, 5> receive_references{{
        {"Ju7hI9zNO+j0", "VE7NHW", " HEARTBEAT SNR", "-17"},
        {"IXXfezaRO+cm", "KG4UHM/6", " SNR", "-03"},
        {"IXXfezaRO+a8", "KG4UHM/6", " SNR", "-24"},
        {"IXXfezaRO+Le", "KG4UHM/6", " HW CPY?", ""},
        {"IXXfezaRO+Mm", "KG4UHM/6", " 73", ""},
    }};
    std::size_t rx_passes = 0;
    for (const auto& reference : receive_references) {
      const auto parts = js8core::protocol::varicode::unpack_compound_message(
          reference.symbols, nullptr, nullptr, nullptr);
      if (parts.size() < 3) continue;
      const bool identity_ok = parts[0] == reference.origin;
      const bool operation_ok = parts[2] == reference.operation;
      const bool number_required = reference.numeric_value[0] != '\0';
      const bool number_ok = !number_required ||
                             (parts.size() > 3 &&
                              parts[3] == reference.numeric_value);
      if (identity_ok && operation_ok && number_ok) ++rx_passes;
    }
    check(rx_passes == receive_references.size(),
          "all 5 compound-command receive wire references");
  }

  {
    // Every public frame decoder must reject malformed base-64 wire symbols
    // rather than interpreting unpack72bits()'s historical zero sentinel as
    // a legitimate heartbeat, compound, directed, or data payload.
    using namespace js8core::protocol::varicode;
    const std::string good = pack_compound_frame("K1ABC", 0, 32767, 0);
    check(good.size() == 12 && !unpack_heartbeat_message(good, nullptr, nullptr, nullptr).empty(),
          "known valid zero-grid heartbeat still decodes");

    bool malformed_rejected = true;
    std::vector<std::string> bad = {
        "", std::string(11, '0'), std::string(12, '#'),
        std::string(12, ' '), good + " "
    };
    for (char invalid_char : {'#', '/', '?', '.'}) {
      for (std::size_t i = 0; i < good.size(); ++i) {
        auto changed = good;
        changed[i] = invalid_char;
        bad.push_back(changed);
      }
    }
    for (auto const& wire : bad) {
      malformed_rejected &=
          unpack_heartbeat_message(wire, nullptr, nullptr, nullptr).empty() &&
          unpack_compound_message(wire, nullptr, nullptr, nullptr).empty() &&
          unpack_compound_frame(wire, nullptr, nullptr, nullptr).empty() &&
          unpack_directed_message(wire, nullptr).empty() &&
          unpack_data_message(wire).empty() &&
          unpack_fast_data_message(wire).empty();
    }
    check(malformed_rejected, "all frame decoders reject invalid and truncated wire symbols");

    // Public decoders consume a valid 12-symbol prefix and ignore trailing
    // bytes, matching the Android-port's legacy behavior.
    const auto with_trailing = good + "#extra";
    std::uint8_t first_tail = 0, padded_tail = 0;
    check(unpack72bits(good, &first_tail) ==
              unpack72bits(with_trailing, &padded_tail) &&
          first_tail == padded_tail &&
          unpack_heartbeat_message(good, nullptr, nullptr, nullptr) ==
              unpack_heartbeat_message(with_trailing, nullptr, nullptr, nullptr) &&
          unpack_compound_frame(good, nullptr, nullptr, nullptr) ==
              unpack_compound_frame(with_trailing, nullptr, nullptr, nullptr),
          "public frame decoders accept trailing bytes after valid prefix");
    int used = 0;
    const auto data = pack_data_message("HELLO", &used);
    const auto fast = pack_fast_data_message("HELLO", &used);
    check(!data.empty() && !fast.empty() &&
          !unpack_data_message(data).empty() &&
          !unpack_fast_data_message(fast).empty() &&
          unpack_data_message(data + "#") == unpack_data_message(data) &&
          unpack_fast_data_message(fast + "#") == unpack_fast_data_message(fast),
          "normal and fast data decoders also ignore trailing bytes");

    bool unsupported_types_rejected = true;
    for (std::uint64_t type = 3; type < 8; ++type) {
      auto wire = pack72bits(type << 61, 0);
      unsupported_types_rejected &=
          unpack_compound_frame(wire, nullptr, nullptr, nullptr).empty();
    }
    check(unsupported_types_rejected, "compound-frame decoder rejects unrelated type tags");

    std::uint8_t remainder = 0xff;
    check(unpack72bits(std::string(12, '#'), &remainder) == 0 && remainder == 0,
          "legacy unpack72bits resets outputs for invalid wire input");
  }

  {
    using js8core::protocol::varicode::pack_pwr;
    bool valid = false;
    auto const normal = pack_pwr("20", &valid);
    check(valid && normal == 100, "power fits in one byte without truncation");

    bool overflow_valid = false;
    auto const overflow = pack_pwr("30", &overflow_valid);
    check(overflow_valid && overflow == static_cast<std::uint8_t>(1000),
          "legacy power helper narrows 1000 mW to eight bits");

    bool trailing_valid = false;
    auto const trailing = pack_pwr("10garbage", &trailing_valid);
    check(trailing_valid && trailing == 10,
          "legacy power parser accepts numeric prefixes");
    bool malformed_valid = true;
    check(pack_pwr("garbage", &malformed_valid) == 0 && !malformed_valid,
          "nonnumeric power input remains invalid");
  }

  {
    using js8core::protocol::varicode::pack_num;
    bool valid = false;
    check(pack_num("-31", &valid) == 33 && valid, "minimum signed number encodes");
    check(pack_num("31", &valid) == 95 && valid, "maximum signed number encodes");
    check(pack_num("12garbage", &valid) == 76 && valid,
          "legacy number parser accepts numeric prefixes");
    check(pack_num("31+5", &valid) == 95 && valid,
          "legacy number parser ignores trailing arithmetic suffix");
    for (auto const& malformed : {"32", "-32", "", "garbage"}) {
      valid = true;
      check(pack_num(malformed, &valid) == 0 && !valid,
            "reject out-of-range or nonnumeric command field");
    }
  }

  if (failures != 0) {
    std::cerr << "varicode_roundtrip failures: " << failures << "\n";
    return 1;
  }
  return 0;
}
