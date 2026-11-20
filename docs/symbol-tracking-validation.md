# Symbol-tracking validation foundation

Symbol tracking is a default-enabled receive fallback. It adds pilot
estimation, fractional symbol extraction and CRC-validated decoding after the
established receive/SIC sequence. Both frequency and timing tracking are enabled
by default in `EngineConfig` and `DecodeParams`. The transmitted waveform and
LDPC/CRC acceptance rules are unchanged. Guarded `tracked_sic` is also enabled
by default and never uses the fixed-waveform reference. Both remain explicitly
configurable; default enablement is a policy choice, not new RF-validation evidence.

## Waveform model

Let `u` be receiver time since the fractional frame start. The transmitter time is

```
q(u) = (1 + clock_ppm * 1e-6) * u + A_t * sin(2*pi*u/P_t)
```

The model integrates each nominal FSK tone exactly in transmitter time. Whole
symbols contribute an integer number of tone cycles; partial symbols contribute
their proportional cycles. The carrier contributes `base_hz * q(u)` cycles. This
produces continuous phase across symbol transitions without resetting oscillators.

Additional receiver-time frequency error contributes the phase, in cycles,

```
offset_hz*u + slope_hz_s*u*u/2
    + A_f * (1 - cos(2*pi*u/P_f)) / (2*pi/P_f)
```

Positive clock ppm advances transmitter time: symbols shorten and all nominal
frequencies increase. Timing wander also affects frequency through `dq/du`;
it is not implemented by shifting boundaries independently of the waveform.
Parameters that allow a nonmonotonic time axis are rejected.

A Gaussian amplitude notch supplies fading. Independent signals are mixed in
floating point with seeded Gaussian noise, then rounded and clipped once to
16-bit PCM. The signal is zero outside its warped frame support. Noise seeds
repeat within a given standard-library implementation; C++ does not require
`normal_distribution` to produce identical samples across different libraries.

This is sampled test audio, not a propagation or calibrated receiver model.
Callers must keep instantaneous frequencies below Nyquist. The current sweep
does so, but the generator does not enforce a bandlimit for arbitrary inputs.

## Running checks

```sh
cmake -S . -B build-symbol-tracking -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-symbol-tracking --parallel 4 --target js8core-tracking-waveform-test js8core-symbol-tracking-test js8core-tracked-extraction-test js8core-tracked-reference-test js8core-tracking-baseline js8core-decoder-roundtrip-test js8core-wav-decode-test js8core-engine-tracking-test js8core-decode-control-test
ctest --test-dir build-symbol-tracking -R 'js8core-(tracking|symbol-tracking|tracked-(extraction|reference|sic)|decoder-roundtrip|wav-tracking|engine-tracking|decode-control)' --output-on-failure
./build-symbol-tracking/js8core-tracking-baseline --sweep 3 > build-symbol-tracking/baseline-release.csv
./build-symbol-tracking/js8core-tracking-baseline --tracking both --sweep 3 > build-symbol-tracking/tracking-paired-release.csv
```

Without arguments the baseline executable checks stationary reception and
noise-only input in all five modes, including normally disabled Ultra. CTest runs
this short check. `--sweep [seed-count]` measures additional impairments, with
three seeds by default. A missed impaired frame is a measurement, not a failing
check. A missed stationary frame or any unexpected payload/type fails the tool.
An intentionally injected interferer's exact payload/type is not unexpected.

With `--tracking frequency|timing|both`, each trial runs the disabled baseline
first, then the selected fallback on identical input. The tool requires every
baseline decode to survive with its exact metadata and precede tracked results.
`--case name` selects one case; `--require-tracked` additionally fails if no frame
was actually recovered by the tracked path. CTest includes a recovery case, so
an implementation that silently bypasses tracking cannot pass all the tests.
`--wrap` rotates the same audio across the 60-second ring boundary; CTest
requires successful tracked recovery in that layout too.

CSV rows contain impairment settings, baseline/enabled target recovery, tracked
frame count, baseline preservation, unexpected frame count and decode-only
elapsed times. Waveform generation is outside those timers. The original
cases use amplitude 5000, noise sigma 1200, base frequency 1501.7 Hz and start
delay plus 13.037 ms. Seed index changes both noise and initial phase. Frequency
and timing wander periods are 10 seconds. These fixed development cases are
not a held-out evaluation or a calibrated SNR sweep.

### Initial development baseline

AppleClang 21, macOS arm64, Release, unchanged receiver, three seeds per mode:

| Case | Target recovered / signal trials |
| --- | --- |
| Stationary | 15 / 15 |
| Constant offset +1.7 Hz | 15 / 15 |
| Constant offset -1.7 Hz | 15 / 15 |
| Drift +0.5 Hz/s | 12 / 15 |
| Drift -0.5 Hz/s | 12 / 15 |
| Clock +1500 ppm | 15 / 15 |
| Clock -1500 ppm | 15 / 15 |
| Smooth wander: 1 Hz and 4 ms | 9 / 15 |
| Drift + clock mismatch + fade | 12 / 15 |
| Drift + clock mismatch + stronger overlapping frame | 9 / 15 |

Total: 129 / 150 target frames. Fifteen additional noise-only trials produced no
frames; no unexpected payloads appeared in any of the 165 trials. This small
dataset does not establish a false-decode probability or prove tracking gains.
Timing figures should be collected without concurrent test/build jobs before
using them for performance comparisons.

## Pilot estimator

`src/decoder/symbol_tracking.{hpp,cpp}` independently implements a bounded
matched-filter search using only the known protocol pilots. It does not use
unknown data-tone decisions or desktop tracker code. The decoder calls it only
when an enabled fallback attempt has a potentially complete received frame.

### Coordinates and measurement

Input is tone-zero-centered complex baseband and an acquired, possibly
fractional, frame-start sample. The sample span must exclude FFT padding.
The estimator shifts the eight-tone bank to be symmetric about zero before
fractional interpolation. This avoids interpolating through the complex Nyquist
wrap in the 12-sample-per-symbol modes. Interpolation uses a normalized 16-tap
Hann-windowed sinc, with exact sample access for integer positions.

Each of the three seven-symbol Costas groups supplies one timing/frequency
anchor. For a trial boundary displacement and residual frequency, each pilot is
correlated against its known tone. Its normalized coherence is

```
|sum(w[n] * x[n] * exp(-j*omega*n))|^2
    / (sum(w[n]) * sum(w[n] * |x[n]|^2))
```

Trapezoidal weights include both boundaries at half weight; interior samples
have unit weight. This references the physical symbol boundaries without the
half-sample offset introduced by left-endpoint-only integration. Cauchy-Schwarz
bounds coherence by one. Per-pilot normalization avoids letting one loud tone
dominate a group. Group scores are averaged over the seven expected tones.

The search covers +/-0.75 tone spacings and +/-0.25 symbol durations relative to
acquisition, using a 9x13 grid, a 5x5 local refinement, and bounded parabolic
peak interpolation. Candidates near the search limits are rejected, not clamped
into apparently valid tracks. Missing interpolation support is unavailable
evidence, never fabricated zero padding.

### Confidence and trajectories

The initial conservative development policy requires:

- Group mean coherence at least 0.5, with at least five pilots at coherence 0.4.
- All three groups accepted; no extrapolation from just one surviving group.
- Timing score drop of at least 0.015 when probing +/-1/8 symbol, to reject
  unobservable/flat timing peaks. Timing and frequency validity are separate.
- Weighted affine fits with maximum anchor residuals of 0.06 tone spacings for
  frequency and 0.04 symbol durations for timing.
- Fitted corrections staying within the search envelope across the whole frame.

These thresholds are independently chosen, dimensionless development limits,
not imported desktop tuning values or RF-validated defaults. Held-out and
recorded-HF evaluation remains outstanding after the requested default switch.

`correction(symbol)` evaluates residual frequency at the symbol center and
timing displacement at the symbol start, in hertz and input samples respectively.
A positive timing correction extracts later; a positive frequency correction
means the received tones are above the nominal tone bank. Invalid components
return zero. The fitted timing slope describes symbol-boundary movement, not
the engine's UTC drift correction. The estimate contains no inter-frame state.

The initial model is affine. Curved wander or contradictory anchors may be
rejected rather than followed; piecewise tracking is outside this milestone.
The estimator unit tests use independently generated complex samples at each
production mode's downsampled geometry. Extraction tests also check all eight
FFT tones, input immutability, missing margins and bit-exact integer bypass.
The paired decoder tool exercises actual audio conditioning and decoding.

## Receive integration and controls

Set `EngineConfig::symbol_tracking` before creating an engine, or
`DecodeParams::symbol_tracking` when calling `legacy_decode()` directly:

```cpp
js8core::EngineConfig config;
config.symbol_tracking = js8core::SymbolTracking::Both;
```

The available values are `Disabled`, `Frequency`, `Timing` and `Both` (default).
There is no runtime engine setter in this change and no change to the virtual
engine interface. The config/parameter fields and `Decoded::tracking_used`
are appended, preserving ordinary source initialization; binary consumers must
rebuild against the updated headers/library.

Every submode completes its original three-pass SIC sequence before optional
tracking runs on that submode's residual audio. The fallback refreshes the
baseband FFT after SIC, considers only potentially complete frames, and spends
at most eight extra acquisition/estimation/decode attempts and eight rescue
attempts per submode/window. These budgets are separate from the original
decoder's rescue budget. It may revisit adjacent +/-half-tone coarse FFT bins
discarded by candidate suppression, within the same eight-attempt limit.
Engine queue-pressure and deadline admission can stop optional work sooner, as
described in the streaming/load section below.
These bounds describe tracking with `tracked_sic` off; the separately enabled
cancellation path may add one equally bounded follow-up pass, as detailed below.

The tracker receives only the actual received sample extent, not the padded FFT
or legacy payload-prefix extent. Channel-bin rounding is accounted for in the
reported frequency. Extraction shifts each symbol's start fractionally and
applies the integrated affine frequency trajectory with a consistent frame
phase reference. There are no allocations inside the per-symbol sample loop;
scratch buffers are allocated per attempt. This is boundary tracking, not a
full within-symbol sample-clock resampler.

Existing LDPC, hard-error, sync and CRC acceptance conditions remain in force.
Existing payload/type successes are not replaced by a tracked version. Only
new, CRC-validated successes emit `Decoded` with `tracking_used = true`.
Reported frequency refers to the nominal acquired frame-start epoch; `xdt`
includes the estimated frame-start timing correction, not the accumulated
end-of-frame timing displacement. SNR uses the existing expression with tracked
tone magnitudes; it is not independently calibrated.

With `tracked_sic` off, tracked successes do not feed another SIC pass. With it
on, qualifying successes can feed the separate guarded reconstruction path
described below. Tracked successes cannot reach fixed-reference subtraction.

### Paired development results

AppleClang 21, macOS arm64, Release, three seeds/mode, all original cases plus:

| Added development case | Disabled target recovery | Both enabled |
| --- | --- | --- |
| Drift totaling 1.4 tone spacings/frame; amplitude 4500, noise sigma 10000, base 1508.125 Hz | 7 / 15 | 10 / 15 |
| Clock +5000 ppm; amplitude 1500, noise sigma 5000 | 14 / 15 | 14 / 15 |
| Gated single tone (no JS8 pilot/data sequence) | 0 frames / 15 trials | 0 frames / 15 trials |

The original signal cases remain at 129 / 150 recoveries in both paths.
Across the expanded matrix, target recovery increased from **150 / 180 to
153 / 180**, with three additional tracked frames (Fast seeds 1 and 2; Turbo
seed 2). All baseline decoded metadata was preserved and no unexpected frames
appeared in the 210 trials, including 30 noise-only/single-tone trials. An
additional 500 noise-only trials across all submodes emitted no frames with
tracking enabled. Frequency-only recovered one additional frame on the added
drift case; timing-only showed no gain on the added clock case.

The added cases were selected during development to exercise successful tracked
decoding. They are not held-out sensitivity evidence. In particular, the initial
affine tracker has not improved the original nonlinear-wander cases. Broader
recorded-HF coverage, RF trials, broader negative tests and target-device latency
measurements remain necessary for production confidence, despite the requested
default enablement.

At the initial integration milestone, all 25 Release CTest tests passed; all
nine focused Debug decoder/tracking tests passed; all eight tracking tests
passed under AddressSanitizer and UndefinedBehaviorSanitizer. The estimator and
extraction executables contain 390 and 2,902 always-active checks respectively.
Changed C++ sources passed `-Wall -Wextra -Wpedantic` syntax checks on AppleClang.
These results do not cover other compilers/platforms or hardware operation.

## External WAV preservation

`js8core-wav-decode-test` now accepts the same tracking modes as the synthetic
tool, plus `--wrap`. Enabled modes run the disabled decoder first on identical
input and require its entire event sequence to remain an exact prefix of the
enabled sequence: payload, type, mode, UTC, SNR, frequency, DT, quality and clock
metadata must all match. Any additional events must be uniquely identified as
tracked successes. Frozen fixture counts are still checked against the disabled
baseline. With `--records`, the existing full-record format is unchanged and
fixture identity checks remain the external comparator's responsibility.

Paired summaries go to stderr as tab-separated rows:

```
PAIR  filename  baseline_events  enabled_events  tracked_events  baseline_ms  enabled_ms
```

Example with the pinned fixtures already downloaded by native CI:

```sh
./build-symbol-tracking/js8core-wav-decode-test --tracking both A build/wav-fixtures/A_*.wav
./build-symbol-tracking/js8core-wav-decode-test --tracking both E build/wav-fixtures/E_*.wav
./build-symbol-tracking/js8core-wav-decode-test --tracking both --wrap A build/wav-fixtures/A_*.wav
./build-symbol-tracking/js8core-wav-decode-test --tracking both --wrap E build/wav-fixtures/E_*.wav
```

The nine fixtures are pinned to
`f1021126bdf9c4979c7213515b91f421d820a261`, as in
`.github/workflows/ci.yml`. Local copies were verified unchanged against that
revision. Their frozen counts are 26 Normal-mode events plus two Slow-mode
events. This step checks JS8Core disabled-versus-enabled preservation and the
existing desktop-derived count expectations; it does not rerun the desktop
decoder or establish new desktop sensitivity parity.

| Local build / enabled mode | Ordinary layout | Wrapped layout |
| --- | --- | --- |
| Release / frequency only | 28 baseline events preserved | 28 preserved |
| Release / timing only | 28 preserved | 28 preserved |
| Release / both | 28 preserved | 28 preserved |
| Debug / both | 28 preserved | 28 preserved |
| ASan + UBSan / both | 28 preserved | 28 preserved |
| Release / both with tracked SIC | 28 preserved, zero cancellations | 28 preserved, zero cancellations |

There were no additional tracking events in these fixtures. Release `--records`
stdout also matched byte-for-byte with tracking disabled and enabled. This is
external compatibility evidence, not an improvement claim or an RF test.
Native CI now runs enabled and wrapped fixture preservation in Debug and
Release, in addition to its existing disabled checks.

The test-only WAV reader validates RIFF/chunk bounds, odd-chunk padding, PCM
format, sample alignment and the 60-second input limit before decoding. It no
longer interprets unsupported formats as PCM or reads an odd PCM byte count into
an undersized sample vector. Only actual file samples count toward a received
window; FFT padding is not advertised to tracking as received audio.
`tools/wav_tracking_test.py` exercises the CLI, paired summaries, frozen count
gates and malformed inputs. CTest registers it when Python 3 is available.

At the WAV-only milestone, all 26 Release CTest tests passed. The WAV CLI/input
test and the paired external fixture checks also passed in Debug and under
ASan + UBSan. Production receiver algorithms were unchanged in this step.

## Experimental trajectory-aware cancellation

`EngineConfig::tracked_sic` / `DecodeParams::tracked_sic` is independently
configurable (default true). It is inert unless `symbol_tracking == Both`. A tracked
CRC success must have valid frequency **and** timing fits before it can supply
a reference. A successful decode without both fits is still reported normally,
but cannot be canceled.

```cpp
config.symbol_tracking = js8core::SymbolTracking::Both;
config.tracked_sic = true;
```

### Reference derivation

`src/decoder/tracked_reference.{hpp,cpp}` is independently written from the
continuous-phase FSK model and weighted least squares, not desktop tracker or
cancellation code. Let `e` be the nominal acquisition epoch in full-rate samples,
`S` the nominal symbol length, `d` the fitted timing offset and `h` the timing
change in samples per symbol. The actual boundary of symbol `k` is

```
b(k) = e + d + k * (S + h)
```

For a receiver sample `n`, symbol progress is `(n - e - d)/(S + h)`. Integrate
the decoded tone numbers over this progress: every whole symbol contributes its
integer tone number in cycles, and the current symbol contributes its fractional
part. With lowest-tone frequency `f0` at epoch `e` and frequency change `g` in
hertz per nominal symbol, the additional carrier cycles are

```
(f0 / Fs) * (n - e) + (g / (2 * Fs * S)) * (n - e)^2
```

Exponentiating the total phase gives a unit analytic reference with continuous
phase across transitions and fractional frame support. This models clock-induced
tone-spacing changes as well as moving boundaries. Constant channel phase is
left for the channel fit. The renderer rejects partial frames, nonfinite or
nonmonotonic trajectories, invalid tones, and frequencies outside real-audio
Nyquist. The helper tests compare it against the independent impaired-waveform
generator across all five submodes, both drift directions and both clock signs.

### Channel fit and guards

For each overlapping four-symbol Hann window, solve the two-coefficient real
least-squares problem

```
min(a,b) sum(w[n] * (audio[n] - a*real(reference[n]) - b*imag(reference[n]))^2)
```

This fits unknown channel amplitude/phase without reusing the fixed-reference
subtraction path. Adjacent windows overlap by half their length; partitioned
weighted blending yields a smooth cancellation waveform and can follow slow
fades. Ill-conditioned fits and weak local projections are discarded. The local
projection gate requires explained weighted energy at least
`max(0.01, 16/weighted_sample_mass)` of input energy. These are conservative
development heuristics, not false-acceptance probabilities.

Cancellation is a pure candidate operation: the caller's audio is unchanged
unless the returned residual is finite and removes at least **3% of total
window energy**. Invalid references, nonfinite samples or insufficient reduction
return no replacement audio. Even an accepted energy reduction is **not proof**
that every undecoded neighboring signal is unharmed; the CRC prerequisite,
controlled overlap tests and the remaining validation limits remain important.

### Ordering, budgets and diagnostics

The established SIC sequence and the complete original optional tracking pass
finish before this path changes any audio. Their events and metadata must remain
an exact prefix when tracked SIC is enabled. Up to two newly tracked, CRC-valid
references are tried per submode/window. Only committed reductions count toward
`DecodeFinished::tracked_cancellations` (summed across the active submodes).

If a cancellation is committed, one additional tracking pass refreshes the FFT
and candidate search on the residual. It has its own eight-attempt/eight-rescue
budgets. There is no recursive cancellation cascade: follow-up results cannot
trigger another subtraction pass. Maximum optional tracking work is therefore
16 acquisition/estimation/decode attempts, 16 rescues and two cancellation
candidate fits per submode/window with this feature enabled. Models and residuals
are local to the decode window; capture-ring samples are never modified.

New CRC results on that residual have `Decoded::after_tracked_sic = true` as well
as `tracking_used = true`. These appended event fields do not change variant
ordering; binary consumers must rebuild against the updated headers/library.
Follow-up SNR and quality use the residual's existing decoder expressions, not
an independently calibrated measurement of the original audio.

### Controlled development evidence

The helper executable has **188 always-active reconstruction/cancellation
checks**, including phase agreement, fractional support, energy reduction,
fading, invalid inputs and rejection of noise/disjoint references. In its exact-
trajectory overlap cases, target reconstruction error fell by at least 97% and
the weaker signal's measured amplitude stayed within 8% of its pre-cancellation
value. These bounds apply to those controlled cases, not arbitrary HF channels.

One independently generated Ultra case exercises the complete estimated-track
pipeline: a frame drifts by 1.4 tone spacings, with a second payload about 9 dB
weaker, offset by 0.7 tone spacings and 30 ms. Across eight development seeds,
the disabled decoder recovered neither frame, ordinary tracking recovered the
stronger frame in all eight trials, and tracked SIC additionally recovered the
weaker frame in four trials. All eight cancellations passed the energy guard.
The follow-up and wrapped-ring regression tests require a real additional CRC
frame; simply emitting a cancellation statistic cannot satisfy them.

On the expanded all-mode 225-trial development sweep, all disabled and ordinary
tracking events/metadata were preserved. There were four committed cancellations
and one additional follow-up payload. An additional 500 noise-only and 100
single-tone trials emitted no frames and committed no cancellations. The nine
pinned external WAV fixtures still preserve all 28 events with tracked SIC
enabled, in ordinary and wrapped layouts; none qualifies for tracked subtraction.

Reproduce the successful overlap regression with:

```sh
./build-symbol-tracking/js8core-tracking-baseline --tracking both --tracked-sic --sweep 8 --mode I --case tracking-overlap --require-followup
```

The synthetic tool records all three stages (disabled, tracking-only, tracking
with SIC). `--require-cancellation` fails if no reduction was committed and
`--require-followup` fails if no additional residual frame was decoded. The WAV
tool also accepts `--tracked-sic`, checks tracking-only prefix preservation, and
prints `SIC\tfilename\tcommitted_count` alongside its existing paired summaries.

Local checks for this milestone: all **31 Release CTest tests passed**, along
with seven focused Debug decoder/SIC/WAV tests and six focused tests under
ASan + UBSan (UBSan configured to halt on error). No desktop implementation code
was consulted for this change. Recorded-HF overlap tests, target-device latency
and broader platform validation are still outstanding.

## Default configuration and opt-out

Both public configuration structures now default to `SymbolTracking::Both` and
`tracked_sic = true`. Existing callers that omit these fields receive tracking
and guarded cancellation when rebuilt against this version. To retain the
historical decoder, explicitly set:

```cpp
config.symbol_tracking = js8core::SymbolTracking::Disabled;
config.tracked_sic = false;
```

To retain tracking but disable its cancellation pass, leave tracking at `Both`
and set only `tracked_sic = false`. The CRC, fit, energy and work-budget guards
are unchanged by the default switch.

The synthetic and WAV comparison tools deliberately force their historical
baseline to `Disabled` / false; the library default change must not contaminate
those comparisons. `tracking_baseline --library-defaults` selects the actual
public defaults for its candidate run. Compile-time checks cover both public
configuration structures, and the `js8core-tracking-defaults` regression requires
an additional CRC-valid follow-up frame from the default-enabled SIC path.

After the default switch, all 32 Release CTest tests passed. The default-enabled
decoder round-trip, default follow-up and WAV-tool tests passed in Debug; the
default follow-up and WAV-tool tests also passed under ASan + UBSan.

## Streaming integration, admission and stale-work hardening

The engine now admits optional tracking/cancellation work only while there is no
newer pending decode snapshot and its monotonic deadline has not expired.
`EngineConfig::optional_decode_budget_ms` defaults to **250 ms**, measured from
the pending snapshot's enqueue time, including queue wait, snapshot copying,
mandatory work and host callback time. Values are clamped to 0..10000 ms; zero
denies optional work. Replacement/coalesced snapshots get a fresh enqueue time.
This changes neither the default tracking/SIC flags nor their acceptance gates.

The existing queue keeps at most one pending snapshot in addition to active
work. Replacement retains only still-active, non-overwritten submode windows
from the same capture epoch and RX session. A monotonic sample counter records
the oldest requested window's absolute start: ring-overwrite validity is checked
against that start, **not** against the later enqueue/sample-end position. The
snapshot copy, active decoder and event publisher use this horizon consistently.

Optional denial is latched for the rest of a decode call. Checkpoints cover
candidate acquisition, pilot grid search, symbol extraction, LDPC attempt
boundaries, reference rendering, local channel fits and residual commit. No
partial search or residual is published as a successful result. Mandatory
decoding is not stopped by the optional deadline or queue pressure. Explicit
cancellation (stop/restart, capture realignment or overwritten input) can stop
mandatory work too. These controls are cooperative: they cannot preempt a vendor
FFT, an individual BP solve or an already executing host callback. This is not a
hard real-time latency guarantee, and host callbacks must remain nonblocking.

Capture epochs are now attached to queued decoder/spectrum events as well as
work requests. A timestamp gap, rate/clock realignment or RX restart invalidates
old-context work and queued events. Ordinary valid queued frame results retain
their existing backpressure protection while their context remains current.
Capture never waits on event consumers. A callback already executing when the
context changes cannot be revoked.
Audio adapter buffer/error callbacks are tagged with their start-session
generation too. Retired callbacks are ignored, and in-flight capture keeps its
entry generation through publication rather than borrowing a restarted session.
Adapters must still stop invoking callbacks before the engine is destroyed;
generation checks are not an object-lifetime substitute.

`DecodeFinished::optional_work_skipped` reports a policy denial;
`DecodeFinished::canceled` reports cooperative cancellation to direct controlled
decoder callers. The engine suppresses stale-context results instead of sending
their completion into a new context. Direct callers may use
`decode_with_control(state, emit, DecodeControl)` for nonblocking optional/cancel
policies. Empty controls allow all work. Callback exceptions fail closed. The
original two-argument `legacy_decode()` remains unchanged and unambiguous for
function-pointer bindings, and intentionally has no engine wall-time budget for
offline verification.

### Automated streaming and overload checks

`tools/engine_tracking_test.cpp` exercises real `Js8Engine::submit_capture()`
conditioning, slot scheduling, decode worker and public event callbacks:

- default-enabled frequency/timing tracking and a real post-SIC CRC recovery;
- successful fresh reception after stop/restart;
- ignored retired audio-adapter buffer/error callbacks, without consuming the
  restarted session's first-buffer marker, followed by positive adapter reception;
- cancellation of in-flight old-session and pre-discontinuity requests;
- suppression of old decode events already queued behind a blocked consumer;
- zero and elapsed nonzero deadlines preserving mandatory positive decodes;
- 32 seconds of accelerated crowded MULTI audio with a deliberately held worker,
  draining as one active plus one coalesced pending decode;
- 61 seconds of accelerated input, rejecting an old partially overwritten
  window and draining only the newest pending request, with all four standard
  submodes retained and no spurious decoded frames.

Positive quality tests allow a 10000-ms optional budget so instrumentation speed
does not select the algorithm under test. Tracking and SIC use their public
enabled defaults. An eight-seed pool avoids dependence on one implementation's
`normal_distribution` noise draw; the test requires both CRC payloads from the
same streaming attempt. Deadline/pressure cases test admission separately.
Reported feed times exclude waveform construction and are not measurements of
an actual audio device's callback deadlines.

`tools/decode_control_test.cpp` checks mandatory metadata preservation, sticky
denial/cancellation, exceptions, interrupted pilot searches, symbol extraction,
reference rendering and partially assembled residual rejection. No caller audio
is modified on interruption. Both tests are part of native Debug/Release CI.

Local validation: the complete Release suite passes **34/34 tests**. Five focused
streaming/control/lifecycle tests pass in Debug, ASan + UBSan, and ThreadSanitizer
(RelWithDebInfo), all on macOS/AppleClang. GCC/Linux and Android/other
target-device runs were not performed here and are not claimed. Recorded-HF/multipath
and physical RF validation remain outstanding. The broader desktop modem gaps
are separate from this tracking/load-hardening change.
