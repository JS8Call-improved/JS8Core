#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace js8core::decoder {

// Maps a six-bit JS8 alphabet character to its word value (0-63), or 0xFF when
// the character is not part of the alphabet.
std::uint8_t word_for(char c);

// CRC-12 used by the JS8 message layer: polynomial 0xC06 over the 11-byte
// block (which already contains the twelve trailing zero bits), final XOR 42.
std::uint16_t crc12(std::uint8_t const* data, std::size_t size);

bool crc_ok(std::array<std::int8_t, 87> const& payload);

// Reconstructs the twelve-character frame text from a CRC-valid payload.
std::string decode_message(std::array<std::int8_t, 87> const& payload);

// Sum-product belief propagation over the 174/87 LDPC code. Returns the number
// of hard-decision disagreements for a valid codeword, or -1 when decoding
// does not converge.
int belief_propagation(std::array<float, 174> const& llr,
                       std::array<std::int8_t, 87>& payload,
                       std::array<std::int8_t, 174>& codeword,
                       int max_iterations = 60, bool early_abort = true);

// True when the generator parity matrix has a one at (row, column).
bool parity_bit(std::size_t row, std::size_t column);

}  // namespace js8core::decoder
