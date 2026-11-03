# JS8 wire compatibility and table provenance

This document distinguishes **on-air identities** from derived implementation
indexes and independently designed DSP coefficients. Its purpose is to make
source auditing and interoperability testing reproducible; it is not a legal
opinion that every copied compilation is free of copyright.

## Retained, unchanged protocol identities

The following values are part of, or needed to reproduce, the interoperable
JS8 wire format. **Do not reorder or alter them for source-code cosmetic
reasons.** Even a one-bit change may break an ordinary JS8Call transmitter or
receiver.

| Location | Wire identity | Reason retained |
| --- | --- | --- |
| `src/decoder/decoder_tables.hpp:kParityRows` | LDPC generator rows | Sets transmitted parity bits |
| `src/decoder/decoder_tables.hpp:kBitChecks` | Sparse LDPC check adjacency | Defines equivalent standard code validity checks |
| `src/decoder/decoder_tables.hpp:kAlphabet` | Six-bit message alphabet | Maps coded values to characters |
| `src/protocol/costas.cpp` | Original and modified Costas sequences | Sync tone numbers are on-air symbols |
| `src/protocol/submode.cpp` and mode profiles | Symbol samples, starts, periods | Defines standard JS8 submodes |
| `src/protocol/varicode_tables.hpp` | Huffman/varicode codes, directed command numbers, group-call assignments and frame identifiers | Defines interoperable message bit patterns |
| `src/protocol/jsc_map.cpp.inc` | 262,144 index-to-word assignments | An index change alters the compressed JSC bitstream |
| `src/protocol/jsc_data.hpp` | JSC word/index layout | Required to interpret the dictionary above |

The JSC indexed dictionary is a *compatibility corpus*, not newly authored
prose. The historical dictionary compilation's source was JS8Call/Jordan
Sherer's JSC material. Whether a particular dictionary compilation has
separate protected selection or arrangement, and how it may be distributed,
must be cleared independently. This document does **not** purport to grant
third-party rights.

## Non-wire policy, now isolated

`src/protocol/varicode_tables.hpp` retains wire-visible alphabets, compressed
symbol assignments, addressed command IDs, group-call numbers and frame types.
Those are not changed by this separation.

`src/protocol/varicode_policy.hpp` independently expresses application-level
decisions using named command behaviors instead of copying the previous
numeric sets:

- whether a recognized directed command is accepted;
- whether it expects an automatic response;
- whether a command ID is one of the explicit buffering operations; the public
  `is_command_buffered()` helper also recognizes any **known** command spelling
  containing a literal space, matching desktop JS8Call's behavior;
- whether that following payload receives a 16-bit checksum;
- how a displayed dBm value maps onto the rounded 1/2/5 milliwatt ladder.

The power reference ladder is generated from decade arithmetic. An unused
dBm-to-label presentation table was removed. None of these decisions is a
newly claimed radio protocol allocation: their *behavior* is retained for JS8Call
interoperability, but the implementation is not a copy of the desktop lookup
container. Distinguish behavioral compatibility from authorship and rights.

The `js8core-policy` CTest case checks 38 desktop-compatible command/alias behavior
vectors, all 19 rounded power values, and endpoint/rounding behavior in both
Debug and Release configurations. These tests guard the boundary between
wire identity and host application semantics.

The legacy `commons.h` shim was retired rather than distributed, because it
was an unused copy-like interface to the old decoder globals. The public API
continues through `include/js8core/` and `protocol/constants.hpp`.

## Derived rather than copied

- `kCheckNodes` is generated `consteval` from the retained `kBitChecks`
  adjacency; the old duplicate table was removed. Each output check node
  lists exactly the bits associated with that parity equation, ordered by
  codeword bit index. A compile-time assertion verifies permitted degrees.
- JSC text lookup now builds a sorted index of the retained `JSC_MAP` entries
  on first use. It uses longest-prefix matching and the original numeric
  dictionary IDs. The independently generated lookup was compared with the
  old two-table lookup on 4,004 exact and prefix inputs across the full
  262,144-index range; every result matched. The frozen FNV-1a compatibility
  digest is `0x9e32712d` and is checked by `verify_jsc_roundtrip`.
  The old approximately 6.3-MB `JSC_LIST` table and 103-entry prefix table
  have been removed. The retained `JSC_MAP` is not reindexed.
- `src/dsp/resampler.cpp:make_js8_fir()` now designs a 97-tap, symmetric,
  0.44-Nyquist-factor Blackman-windowed sinc low-pass for integer-ratio
  resampling, normalized to unity DC gain. The old desktop-specific FIR
  coefficient array was removed. This is a receiver-side implementation
  choice, not an over-the-air protocol identity; the TX symbol sequence is
  unchanged.

## Verification requirements

For changes to any retained protocol table or indexing algorithm:

1. Run CTest in both Debug and Release, including always-active clock and application-policy checks, JSC single-character and
   30,000 multi-character compression round trips and the indexed-lookup
   digest, as well as CRC, varicode and synthetic modem frames.
2. Run the pinned desktop JS8Call WAV fixture matrix and match the recorded
   decode counts.
3. If changing the RX filter, check 3-kHz passband magnitude is within 1%
   and 8-kHz image rejection is greater than 60 dB, and repeat the recorded
   sample parity comparisons after resampling.
4. If changing generator/LDPC/Costas/Huffman/JSC index values, additionally
   compare independent standard JS8 audio and frame vectors. A green
   self-round-trip alone is insufficient to prove interoperability.

Keep independent authorship and third-party license attribution in the
release review. Compatibility is a technical requirement, not a legal
determination.
