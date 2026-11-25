# Timing fixes: validation

Three changes in `src/decoder/legacy_decoder.cpp`, all about where in time the
decoder looks for a frame. The transmitted waveform, the LDPC decoding and the
CRC acceptance rule are unchanged.

1. **Candidate start time without its lag.** The candidate spectra fill the
   whole 2-symbol FFT span under a window that peaks one symbol in, so a tone
   scores best when the spectrum starts 2 coarse steps (half a symbol) before
   it. Candidate times were taken from the window start plus half a step, so
   they came out 1.5 steps early: Normal -0.043 s, Fast -0.039 s, Turbo
   -0.020 s, Slow -0.107 s. They now count from the true step.
2. **A second time fit.** After the frequency fit, time is searched again at
   the corrected frequency over half the first window, as WSJT-X does in
   `ft8b`.
3. **The whole frame is read.** The decoder read the downsampled channel only
   to `payload_samples` (79 symbols) counted from the start of the period. A
   frame starts `start_seconds` plus its time offset into the period, so its
   last symbols read as silence: at Normal speed and a 0.3 s offset, 5 of the
   21 sync tones; above about 0.62 s, data tones too. Normal, Fast and Turbo
   now read to 2,812 samples, capped at the channel length, as WSJT-X FT8 and
   JS8Call-improved v3.0.3 do. Slow and mode I keep the old length: at Slow
   the longer read gained 0.7% messages for 15% to 20% more CRC-check chances.

These changes were first made and measured in a fork
(https://github.com/RobBobbins/JS8Core, commits `4f21eb5` and `4e9ecd2`). This
branch carries only them, on top of `feaec6f`.

## Test audio

48 sets of computer-made JS8 audio. One list of 700 messages (100 each of
heartbeat, CQ, contact reply, directed short text, relay, ragchew and weather
report, from 40 stations) is built into frames by JS8Core's
`build_message_frames` and sent at all four speeds and 8 levels (-6 to -27 dB
in 3 dB steps). A message of several frames goes out in consecutive cycles at
one frequency and start time, like a station sending it.

- Kinds: no fading, 4 stations per file; ITU mid-latitude disturbed fading
  (1 Hz spread, 2 ms delay), 4 stations per file; crowded band (60 stations
  per file at Normal and Slow, 37 at Fast, 19 at Turbo).
- Variants of each: as made; drift up to 0.25 Hz/s (`_d`); clock off by up to
  1 s (`_c`); both (`_dc`).
- In total 718,880 frames and 268,800 messages. A message counts as decoded
  only when every one of its frames is.
- Limits of this audio: it was made by the fork's own generator and JS8Core's
  own modulator, so it is not independent of JS8Core; and it spreads frames
  evenly over speeds and levels, which a real band does not. The audio
  (44.7 GB) and the generator are available on request.

Each file was decoded with `js8core-wav-decode-test --records` (tracking
disabled, as in the desktop comparison), with modes B and C added to a
measurement copy of the test program (not part of this branch).

## Results, tracking disabled

Right frames are frames in the list that was sent; wrong frames are decoded
frames that were not sent.

| Speed | Right frames, `feaec6f` | This branch | Whole messages, `feaec6f` | This branch | Wrong frames |
|---|---:|---:|---:|---:|---:|
| Normal | 98,057 | 100,996 (+3.0%) | 34,450 | 35,881 (+4.2%) | 0 → 0 |
| Fast | 75,672 | 87,046 (+15.0%) | 25,163 | 30,181 (+19.9%) | 0 → 0 |
| Turbo | 47,191 | 64,013 (+35.7%) | 14,403 | 21,386 (+48.5%) | 0 → 0 |
| Slow | 99,293 | 103,317 (+4.1%) | 34,728 | 36,760 (+5.9%) | 0 → 0 |
| **All** | **320,213** of 718,880 | **355,372 (+11.0%)** | **108,744** of 268,800 | **124,208 (+14.2%)** | **0 → 0** |

- By kind, all speeds: no fading +6.5% right frames, fading +12.8%, crowded
  +15.1%.
- Frames gained and lost: 36,983 right frames decoded only by this branch;
  1,824 decoded only by `feaec6f` (Normal 391, Fast 282, Turbo 163, Slow 988).

## Results, tracking on

The 12 sets with both drift and clock offset (`_dc`), decoded with
`--tracking both` (the default configuration):

| Speed | Right frames, `feaec6f` | This branch | Whole messages, `feaec6f` | This branch | Wrong frames |
|---|---:|---:|---:|---:|---:|
| Normal | 24,361 | 25,150 (+3.2%) | 8,548 | 8,934 (+4.5%) | 0 → 0 |
| Fast | 18,848 | 21,780 (+15.6%) | 6,306 | 7,554 (+19.8%) | 0 → 0 |
| Turbo | 11,651 | 16,004 (+37.4%) | 3,509 | 5,339 (+52.2%) | 0 → 0 |
| Slow | 19,589 | 20,756 (+6.0%) | 6,552 | 7,045 (+7.5%) | 0 → 0 |
| **All** | **74,449** of 179,720 | **83,690 (+12.4%)** | **24,915** of 67,200 | **28,872 (+15.9%)** | **0 → 0** |

On the same 12 sets, tracking adds +2.5% right frames to `feaec6f` (72,602 →
74,449) and +0.6% to this branch (83,215 → 83,690). Tracking and these changes
work on different losses: most of what the timing fixes recover, tracking does
not.

## Processor time

Processor seconds per file (one file = one transmit period), one decoder
process at a time, 5 files per program start, pinned to one CPU, the decoders
taking turns batch by batch. User-space instruction counts (Linux
`perf_event_open`) give the same ratios and do not depend on other load on the
machine. Tracking disabled:

| Set (files) | Speed | `feaec6f` | This branch | Ratio | Instructions ratio |
|---|---|---:|---:|---:|---:|
| 60 stations per file (30) | Normal | 0.217 s | 0.244 s | 1.12 | 1.10 |
| 37 stations per file (30) | Fast | 0.212 s | 0.259 s | 1.22 | 1.18 |
| 19 stations per file (30) | Turbo | 0.113 s | 0.125 s | 1.10 | 1.06 |
| 60 stations per file (30) | Slow | 0.724 s | 0.823 s | 1.14 | 1.14 |
| 4 stations per file (20) | Normal | 0.059 s | 0.064 s | 1.08 | 1.07 |
| Noise only (20) | Normal | 0.023 s | 0.024 s | 1.04 | 1.03 |

With `--tracking both` the ratios are the same (1.02 to 1.20). The largest
share of one core per period is crowded Fast with tracking on: 5.3% for this
branch against 4.4% for `feaec6f` (the test program decodes each file twice in
that mode, once without tracking; both runs are counted).

## The repository's own checks

- CTest, Debug and Release: 35 of 35 tests pass.
- Tracking checks on the 9 reference recordings (`--tracking both`, with
  `--wrap` and `--tracked-sic`): no changed or invalid results.
- Reference decode counts (`kReference` in `tools/wav_decode_test.cpp`) and
  the desktop comparison (`tools/reference/compare_wav_results.py`): all 28
  desktop frames are decoded with matching metadata, and one more:
  `QB+p3UuGBfqK` in `A_2_9.wav` (591.75 Hz, 0.300 s, -17 dB). Neither the
  JS8Call-improved v3.0.3 release nor `feaec6f` decodes it, and the recording
  has no list of what was sent, so whether it is a real frame is not known.
  It contains `uGBfq` at the same place as all 8 frames the desktop decodes in
  that file. Both checks fail on this one frame (A_2_9: 9 decodes, 8
  expected).

## Not tested

- Tracking on for the 36 sets without both drift and clock offset.
- Live on-air audio with this branch alone (the fork's live comparison runs
  its full decoder, which includes these changes).
- Speed on a phone.
