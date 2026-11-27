# Spread reading for fast fading: validation

A budgeted, report-only retry in `src/decoder/legacy_decoder.cpp`
(`attempt_decode`), with its switch `spread_reading` in `DecodeParams` and
`EngineConfig`, and a regression test `tools/spread_reading_test.cpp`. It
reads the tones a second way, used only when the 21 Costas tones show that the
signal's power is spread onto the neighbouring tones. The ordinary passes, the
last try, the other retries, the waveform, the LDPC decoding, the CRC
acceptance rule and the deep sync path are unchanged.

## Why

Fast fading (Doppler spread near or above the tone spacing: 6.25 Hz at
Normal, 3.125 Hz at Slow) spreads each tone's power onto its neighbours. JS8
maps bits straight to tone numbers (no Gray code), so a misread neighbour
costs up to 3 bits. On 10 Hz fading at Normal speed, `c4c6f53` decodes 1 of
2,400 test frames.

## The reading

Each tone is scored by its own energy and its two neighbours' energy:

    score(k) = w0 * E(k) + w1 * (E(k-1) + E(k+1))
    w0 = t0 / (1 + t0) / N        w1 = t1 / (1 + t1) / N

- `E` is the squared FFT magnitude. Tone 0's lower neighbour and tone 7's
  upper neighbour are the FFT bins just outside the 8 tones.
- `N` is the mean energy of the Costas-symbol bins more than one tone away
  from the sent Costas tone.
- `t0` is the centre's signal-to-noise ratio and `t1` the neighbours', both
  measured on the 21 Costas tones.
- Bit LLRs come from all 8 tone scores (log-sum-exp for bit one minus bit
  zero), clamped to ±20.

This is the textbook detector for a Rayleigh-faded tone of known spread;
WSJT-X's Q65 fast-fading decoder weights the bins around each tone the same
way (read from a description of that code; nothing copied). With `w1 = 0` it
is the same score as the noncoherent soft reading pass 2 uses
(`docs/soft-likelihood-validation.md`). This change adds the neighbour term.

## Where it runs

After the ordinary passes, the last try and the adaptive whitening retry have
failed for a candidate, and before the LDPC rescue, aided re-demodulation and
quadratic phase retries.

| Gate | Value |
|---|---|
| Switch | `DecodeParams::spread_reading`, `EngineConfig::spread_reading` (default on) |
| Optional decode budget | `optional_allowed()` before the retry, before the try is counted, and before each result is reported; belief propagation and OSD get the decode control |
| Tries per decode | `kSpreadBudget` = 64 (`RetryBudget::spread`) |
| Spread shown on the Costas tones | `t1` at least `kSpreadMinShare` = 0.2 of `t0` |
| Signal | `t0` at least `kSpreadMinSignal` = 4 |
| Costas tones read right by the spread reading | at least `kSpreadMinSync` = 9 of 21 |

- Belief propagation runs first. If it fails, ordered statistics decoding
  runs on the same reading with the last try's limits, on untracked attempts
  only.
- Both results must pass the last try's checks: CRC, hard-error limits, a
  readable frame, and no different frame nearby.
- Results are not subtracted from the audio, as with the other retries.
- Deep sync attempts reach the retry as they reach the adaptive whitening
  retry, under the optional decode budget.

## How the limits were picked

On the tuning sets `itu_hm_s` and `itu_ld_s` (800 frames, below), the noise
sets of all four speeds, and the Slow drift sets `rs_flat_d` and `rs_crowd_d`,
with a test copy of this branch where each limit could be set.

- **Signal minimum.** With no minimum, the retry ran 17 times on the
  noise-only sets (`noise4`, `bnoise`, `cnoise`, `enoise`: 1,600 files), every
  time with `t0` below 4, and decoded nothing. At 4 it runs 0 times there. On
  `rs_flat_d`, 13 of 2,017 retry successes had `t0` below 4; on the two tuning
  sets, none had `t0` below 6.
- **Costas gate.** Spread-reading Costas tones at least 9 (no ordinary Costas
  minimum beyond the decoder's existing 7): 175 of 800 on the tuning sets.
  Both gates off: 187. Also requiring 9 ordinary Costas matches, as the last
  try does: 153.
- **Tries per decode.** On `rs_crowd_d` (60 stations per file, Slow, drift),
  `c4c6f53` decodes 7,226 right frames. With a budget of 16 the branch
  decodes 7,452, with 32: 7,593, with 64: 7,617, unlimited: 7,618. Decode
  time for the set was 31 to 33 s in all four. On `rs_flat_d`, the most
  tries in one decode was 14.

## Regression test

`tools/spread_reading_test.cpp` (`js8core-spread-reading-test`, in CTest and
the CI build list). It builds a Normal-speed frame in code through a 10 Hz
Gaussian-Doppler fading channel (32 paths, seed 1) with white noise, and
checks:

- the spread reading decodes the frame;
- with the optional budget denied, no frame is reported;
- with `spread_reading` off, the other attempts and retries do not decode it;
- noise alone gives no frame;
- the same frame without fading decodes with the optional budget denied.

Seed 1 was picked from seeds 1 to 20 because it decodes only with the spread
reading at amplitudes 700, 1000 and 1500; the test uses 1000. It passes in
Debug and Release builds.

## Test audio

**Fast-fading sets.** Computer-made JS8 audio through the ITU-R F.1487
two-path (Watterson) model, made by my fork's generator. 25 files per level
at 0, -4, -8 and -12 dB (signal in 2,500 Hz), 4 frames of random text per
file at random frequencies and start times, so 100 frames per level and 400
per set.

| Set | Speed | Channel | Use |
|---|---|---|---|
| `itu_hm_s` | Normal | High-latitude moderate (10 Hz, 3 ms) | Tuning |
| `itu_ld_s` | Normal | Low-latitude disturbed (10 Hz, 6 ms) | Tuning |
| `itu_hd_s` | Normal | High-latitude disturbed (30 Hz, 7 ms) | Test |
| `itu_hm_f`, `itu_hm_g` | Normal | High-latitude moderate | Test (seeds 81, 85) |
| `itu_ld_f`, `itu_ld_g` | Normal | Low-latitude disturbed | Test (seeds 82, 86) |
| `bhm_f`, `bld_f` | Fast | 10 Hz, as above | Test |
| `chm_f`, `cld_f` | Turbo | 10 Hz, as above | Test |
| `noise4`, `bnoise`, `cnoise` | Normal, Fast, Turbo | Noise only, 1,600 decode chances each | Test |

The frame text in these sets is random characters. About a third of random
frames are not readable by any unpacker, and the retry, like the last try and
the other retries, refuses unreadable frames. With that check switched off in
a test copy, the tuning sets give 269 of 800 instead of 175. Real JS8 frames
are always readable, so on real traffic the check costs nothing.

**48 real-traffic sets**, the same as in `docs/second-fine-timing-validation.md`:
700 messages (100 each of heartbeat, CQ, contact reply, directed short text,
relay, ragchew and weather report, from 40 stations), built into frames by
`build_message_frames` and sent at all four speeds and 8 levels (-6 to
-27 dB). Kinds: no fading, ITU mid-latitude disturbed fading (1 Hz, 2 ms),
crowded band; each as made, with drift up to 0.25 Hz/s (`_d`), clock off by
up to 1 s (`_c`), and both (`_dc`). 718,880 frames, 268,800 messages. A
message counts only when every one of its frames is decoded.

Limits of this audio: it was made by my fork's own generator and JS8Core's
own modulator, so it is not independent of JS8Core. The audio and the
generator are available on request.

Each file was decoded with `js8core-wav-decode-test --records`, with modes B
and C added to a measurement copy of the test program (not part of this
branch). Right frames are frames that were sent; wrong frames are decoded
frames that were not sent. Test program defaults otherwise.

## Results on the fast-fading sets

Right frames, all four levels together, tracking disabled. Wrong frames: 0
in every row, both builds.

| Sets | Frames | `c4c6f53` | This branch |
|---|---:|---:|---:|
| Normal, 10 Hz, tuning (`itu_hm_s`, `itu_ld_s`) | 800 | 0 | 175 |
| Normal, 10 Hz, test (`_f`, `_g`) | 1,600 | 1 | 359 |
| Normal, 30 Hz (`itu_hd_s`) | 400 | 0 | 0 |
| Fast, 10 Hz (`bhm_f`, `bld_f`) | 800 | 148 | 306 |
| Turbo, 10 Hz (`chm_f`, `cld_f`) | 800 | 590 | 590 |
| Noise only (`noise4`, `bnoise`, `cnoise`) | 4,800 chances | 0 wrong | 0 wrong |

By level, all six Normal 10 Hz sets (600 frames per level): 0 dB 1 → 200,
-4 dB 0 → 168, -8 dB 0 → 129, -12 dB 0 → 37. Fast 10 Hz (200 frames per
level): 0 dB 61 → 112, -4 dB 47 → 86, -8 dB 34 → 86, -12 dB 6 → 22.

- With `--tracking both`: Normal the same as above; Fast 152 → 310; Turbo
  593 in both builds; 0 wrong.
- With `--deep-sync`: Normal the same; Fast 149 → 307; Turbo 602 in both
  builds; 0 wrong.
- With `--adaptive-sic`: the same as without it; 0 wrong.

## Results on the 48 sets, tracking disabled

| Speed | Right frames, `c4c6f53` | This branch | Whole messages, `c4c6f53` | This branch | Wrong frames |
|---|---:|---:|---:|---:|---:|
| Normal | 105,226 | 105,226 (+0.00%) | 37,463 | 37,463 (+0.00%) | 1 → 1 |
| Fast | 90,422 | 90,422 (+0.00%) | 31,305 | 31,305 (+0.00%) | 1 → 1 |
| Turbo | 62,259 | 62,259 (+0.00%) | 20,445 | 20,445 (+0.00%) | 1 → 1 |
| Slow | 108,862 | 110,891 (+1.86%) | 38,658 | 39,238 (+1.50%) | 3 → 4 |
| **All** | **366,769** of 718,880 | **368,798 (+0.55%)** | **127,871** of 268,800 | **128,451 (+0.45%)** | **6 → 7** |

- 2,029 right frames decoded only by this branch, all Slow; 0 decoded only
  by `c4c6f53`.
- The gain is all on the six Slow sets with drift: `rs_flat_d` +478 frames,
  `rs_flat_dc` +437, `rs_crowd_d` +391, `rs_crowd_dc` +354, `rs_fade_dc`
  +192, `rs_fade_d` +177.
- The new wrong frame: `rs_flat_d`, file `S-18_250.wav` (Slow, no fading,
  drift, sent at -18 dB), frame `674e6d65716c61444e37736e` at 1470.25 Hz,
  reported -28 dB. It comes from the OSD step on the spread reading, at the
  frequency of a sent frame (1470.09 Hz) that neither build decodes.
- Without the OSD step, that file gives no wrong frame, but the gains shrink:
  Slow drift sets 383 frames instead of 2,029, Normal 10 Hz test sets 146
  instead of 359, Fast 10 Hz 265 instead of 306. This branch keeps the step.

## Results with tracking on

The 12 sets with both drift and clock offset (`_dc`), decoded with
`--tracking both`:

| Speed | Right frames, `c4c6f53` | This branch | Whole messages, `c4c6f53` | This branch | Wrong frames |
|---|---:|---:|---:|---:|---:|
| Normal | 26,085 | 26,085 (+0.00%) | 9,275 | 9,275 (+0.00%) | 1 → 1 |
| Fast | 22,399 | 22,399 (+0.00%) | 7,754 | 7,754 (+0.00%) | 1 → 1 |
| Turbo | 15,249 | 15,249 (+0.00%) | 4,978 | 4,978 (+0.00%) | 1 → 1 |
| Slow | 22,125 | 23,048 (+4.17%) | 7,483 | 7,725 (+3.23%) | 1 → 1 |
| **All** | **85,858** of 179,720 | **86,781 (+1.08%)** | **29,490** of 67,200 | **29,732 (+0.82%)** | **4 → 4** |

923 right frames gained (all Slow), 0 lost.

## Processor time

Per file (one file = one transmit period), one decoder process at a time, 5
files per program start, pinned to one CPU, the two builds taking turns batch
by batch. User-space instruction counts (Linux `perf_event_open`) do not
depend on other load on the machine.

| Set (files) | Speed | Instructions, tracking off | Processor time, tracking off | Instructions, `--tracking both` |
|---|---|---:|---:|---:|
| 60 stations per file (25) | Normal | 0.93 | 0.94 | 1.07 |
| 37 stations per file (25) | Fast | 1.00 | 0.98 | 1.00 |
| 19 stations per file (25) | Turbo | 1.00 | 0.97 | 1.00 |
| 60 stations per file (30) | Slow | 1.00 | 1.00 | 1.00 |
| 4 stations per file (15) | Normal | 1.00 | 1.02 | 1.00 |
| Noise only (20) | Normal | 1.00 | 1.06 | 1.00 |

Ratios to `c4c6f53`. On the 60-station Normal set, this branch decodes 314
right frames against 311 with tracking off, and 311 against 314 with
`--tracking both`; both builds decode the same frames on the other sets.

## The repository's own checks

The checks in `.github/workflows/ci.yml` and
`.github/workflows/desktop-interoperability.yml`, run on Linux:

- CTest, Debug and Release: 41 of 41 tests pass, including the new
  `js8core-spread-reading`.
- Reference decode counts (`kReference` in `tools/wav_decode_test.cpp`),
  modes A and E, Debug and Release: pass.
- Coherent likelihood WAV regression, Debug and Release: pass.
- Tracking checks on the reference recordings (`--tracking both`, plain,
  with `--wrap`, and with `--tracked-sic --deep-sync`, modes A and E), Debug
  and Release: pass.
- Package smoke test, Debug and Release: pass.
- Desktop comparison (`tools/reference/compare_wav_results.py`, 3 Hz,
  0.15 s, 6 dB, quality 0.30, `--adaptive-sic --deep-sync` records,
  `--expected-core-only tools/reference/expected_core_only.tsv`): PASS, 8 WAV
  files and all 30 expected records matched.

## Not tested

- Fast-fading sets at Slow speed.
- Tracking on for the 36 sets without both drift and clock offset.
- Live on-air audio with this branch alone.
- Speed on a phone.
