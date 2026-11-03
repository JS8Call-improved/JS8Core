# js8core

An independent, MIT-licensed implementation of the [JS8](https://www.js8call.com/) modem library.

`js8core` is a C++20 library implementing the core of the JS8
digital text/data protocol: the JSC frame codec, varicode framing, the JS8 modem
DSP (resampler + FIR), Costas/submode handling, the JS8 protocol engine, and the
decoder. It ships only **functional protocol data** (the JSC alphabet, JSC symbol
tables, and the on-air protocol constants) together with independently
written implementation code. Receive FIR coefficients and JSC lookup indexes
are now generated rather than copied from desktop source.

## What is JS8?

JS8 is a low-power, text-based digital text/data mode for amateur radio that uses a
8-FSK audio waveform and contains a built-in FEC scheme. JS8 allows
two-way text and low-bandwidth data communication over the air with very weak
signals. This library implements the signal-processing and framing core.

## Authors

`js8core` is authored by Kelly-Shane Fuller
([`punk-kaos`](https://github.com/punk-kaos)).

JS8 is a public amateur-radio digital mode; this library implements that protocol
as an independent, MIT-licensed reimplementation.


## License

`js8core` is released under the **MIT License**. All implementation code in this
repository was written for `js8core`.

Third-party headers bundled under
`vendor/` and linked header-only libraries carry their own licenses:

| Component | License |
|---|---|
| `js8core` source | MIT |
| `vendor/CRCpp/CRC.h` | MIT / custom (standard CRC-16/CRC-32 reference) |
| `vendor/pocketfft/pocketfft_hdronly.h` | BSD-3-Clause (Max-Planck-Society / P. Bell / M. Reinecke) — header-only FFT|
| Eigen (header-only) | MPL-2.0 |
| `<android/log.h>` shim (`vendor/android/`) | MIT (desktop fallback only; real header used on Android) |

See [LICENSE](LICENSE) for the full text.

## Repository layout

```
js8core/
├── include/js8core/     # public headers (the library API)
├── src/                 # independent implementations
│   ├── protocol/        # JSC, split varicode codec modules, submode, Costas
│   │                    #   tables.hpp: wire identities / codebooks
│   │                    #   varicode_policy.hpp: app behavior and power references
│   │                    #   derived JSC text search index
│   ├── decoder/         # legacy_decoder.cpp (JS8 decoder)
│   ├── dsp/             # flatten.cpp, resampler.cpp (JS8 modem DSP + FIR)
│   ├── engine/          # engine.cpp (JS8 protocol engine)
│   └── tx/              # modulator.cpp (JS8 modulator)
├── tools/               # verification helpers (round-trip checks)
├── vendor/              # small shim libraries (see table above)
└── CMakeLists.txt
```


## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target js8core -j
```

Notes:

- The core library builds with the default target. Focused test tools are
  `EXCLUDE_FROM_ALL` and should be built explicitly before running CTest.
- The library links nothing of its own — Eigen and pocketFFT are all
  header-only. The JS8 detection path in `legacy_decoder.cpp` uses
  `vendor/pocketfft/pocketfft_hdronly.h` (BSD-3-Clause) for its FFTs, and
  Eigen (MPL-2.0) only in the spectrum flattener and decoder baseline fit, so the
  library has no external link dependency and no GPL dependency.
- `<android/log.h>` is an external NDK header on Android. A functional
  `vendor/android/log.h` shim provides a desktop fallback (`__ANDROID__` is not
  defined) and otherwise defers to the real NDK header via `#include_next`.

The [protocol data and provenance inventory](docs/protocol-data-provenance.md)
describes which exact on-air tables are retained and which indexes and
receive filters are derived. The legacy `commons.h` compatibility scaffold
is not distributed: consumers use the `include/js8core/` C++ API and
`protocol/constants.hpp` instead. Application-level command behavior and
power presentation have their own internal policy specification in
`src/protocol/varicode_policy.hpp`. Its compatibility decisions are
tested independently of the wire tables.

## Verification

Two focused checks are provided in `tools/`:

- `verify_jsc_roundtrip.cpp` — round-trips the JSC codec: asserts
  `JSC_SIZE == 262144`, and checks single-char and multi-char
  `compress()` → `decompress()` correctness (expected: 41/41 single-char and
  30000/30000 multi-char round-trips).
- `varicode_roundtrip.cpp` — round-trips the varicode framing against the
  desktop reference (57 frames).

```bash
cmake --build build --target js8core-crc-test verify_jsc_roundtrip js8core-varicode-test js8core-policy-test js8core-clock-math-test js8core-vox-test
ctest --test-dir build --output-on-failure
./build/verify_jsc_roundtrip          # prints: PASS
./build/js8core-varicode-test         # prints: 57 OK frames
```

## Audio capture requirements

The decode engine operates internally at 12 kHz. Capture accepts 16-bit
mono or interleaved multi-channel PCM. Integral ratios (including 48 kHz)
use an independently designed 97-tap Blackman-windowed FIR;
fractional ratios such as 44.1 kHz use a streaming,
band-limited 49-tap, 256-phase windowed-sinc resampler. Source buffers
must arrive in order, with `AudioInputBuffer.captured_at` marking the first
frame of each buffer in the steady-clock domain. Discontinuities over 250 ms
trigger a fresh UTC alignment. Capture is serialized internally.


## Platform integration and transmission timing

Outgoing frames are scheduled using `AudioOutputBuffer.playback_at`, the
steady-clock timestamp at which the **first** output sample is expected to reach
the audio device. Without a timestamp, the engine falls back to callback time;
that can introduce a device-buffer latency offset. The fractional TX resampler
uses a windowed-sinc polyphase reconstruction filter to suppress imaging.

If a `RigControl` adapter is supplied, it must report a connected, current
`RigState` in its state callback before TX can begin and report confirmation
after a PTT state change. The engine requests PTT on a control worker and
mutes payload audio until keyed state is confirmed. It requests key-up on TX
completion or cancellation. `RigControl::apply()` must remain nonblocking and
preserve tuning/mode when given an otherwise unchanged state. When no rig
adapter is supplied, PTT is the responsibility of the host application.

RX snapshots are copied on the decode worker in bounded chunks rather than
holding the capture mutex for the full ring. Discontinuous or stale snapshots
are discarded.

## Android-port API compatibility

The public C++ engine, events, frame codec, and callbacks are source-compatible
with the original Android-port JS8Core API. The legacy JNI adapter may supply an
offline rig stub even though its host manages PTT: an explicit
`set_tx_ready(true)` enables that host-managed path without relaxing
confirmed PTT requirements for an online rig. The grant expires when the host
calls `set_tx_ready(false)`, the session ends, or a real rig comes online.

Legacy JNI capture may timestamp callback delivery instead of the first audio
sample. When timestamps move backwards relative to the sample timeline, the
engine switches to contiguous-sample accounting for that receive session.
This prevents accelerated WAV submissions from clearing the capture ring.
Actual forward timestamp gaps still cause realignment while source timestamps
are trustworthy.

The original header `js8core/protocol/time_utils.hpp` is provided with
independently written, wire-compatible time packing helpers. The existing
`js8core::js8core` CMake target is preserved, and Eigen can be located from
a sibling host project's `vendor/` directory.

Run `js8core-android-compat-test` alongside the native regressions to check
external PTT, offline status callbacks, and accelerated audio submission.

## Building and testing the independent package

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2 --target js8core js8core-crc-test js8core-varicode-test verify_jsc_roundtrip js8core-vox-test js8core-fractional-rx-test js8core-decoder-roundtrip-test js8core-engine-smoke-test js8core-rig-test
ctest --test-dir build --output-on-failure
cmake --install build --prefix /tmp/js8core-stage
cmake -S tools/package_smoke -B build/package-smoke -DCMAKE_PREFIX_PATH=/tmp/js8core-stage
cmake --build build/package-smoke
```

The exported target is `js8core::js8core` and may be consumed using
`find_package(js8core CONFIG REQUIRED)`. GitHub Actions now builds Debug and
Release, executes the decoder/engine/PTT tests, and checks an installed
downstream consumer. Independent desktop WAV SNR parity tests should continue
to be maintained before cutting a release.
