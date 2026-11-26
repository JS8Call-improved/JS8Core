# Second fine timing search: validation

One change in `src/decoder/legacy_decoder.cpp`, in `attempt_decode`. After
the frequency search, time is searched again at the corrected frequency, over
half the first window (`origin ± quarter_symbol / 2` downsampled samples), as
WSJT-X does in `ft8b`. The candidate search, the waveform, the LDPC decoding
and the CRC acceptance rule are unchanged.

The first fine timing search runs at the candidate's frequency, before the
frequency is corrected; until now the time it found was kept.

This was split out of pull request #6, without that pull request's change to
the frame read length.

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
- Limits of this audio: it was made by my fork's own generator and JS8Core's
  own modulator, so it is not independent of JS8Core; and it spreads frames
  evenly over speeds and levels, which a real band does not. The audio
  (44.7 GB) and the generator are available on request.

Each file was decoded with `js8core-wav-decode-test --records`, with modes B
and C added to a measurement copy of the test program (not part of this
branch). Right frames are frames in the list that was sent; wrong frames are
decoded frames that were not sent.

## Results, tracking disabled

| Speed | Right frames, `feaec6f` | This branch | Whole messages, `feaec6f` | This branch | Wrong frames |
|---|---:|---:|---:|---:|---:|
| Normal | 98,057 | 99,708 (+1.7%) | 34,450 | 35,303 (+2.5%) | 0 → 0 |
| Fast | 75,672 | 82,480 (+9.0%) | 25,163 | 28,362 (+12.7%) | 0 → 0 |
| Turbo | 47,191 | 53,515 (+13.4%) | 14,403 | 17,124 (+18.9%) | 0 → 0 |
| Slow | 99,293 | 102,779 (+3.5%) | 34,728 | 36,474 (+5.0%) | 0 → 0 |
| **All** | **320,213** of 718,880 | **338,482 (+5.7%)** | **108,744** of 268,800 | **117,263 (+7.8%)** | **0 → 0** |

- By kind, all speeds: no fading +3.5% right frames, fading +6.0%, crowded
  +8.3%.
- Frames gained and lost: 19,789 right frames decoded only by this branch;
  1,520 decoded only by `feaec6f` (Normal 361, Fast 344, Turbo 335, Slow 480).

## Overlap with the coarse timing bias correction

The coarse timing bias correction (a separate pull request) recovers mostly
the same frames. Same 48 sets, tracking disabled:

| Build | Right frames | Whole messages | Wrong frames |
|---|---:|---:|---:|
| `feaec6f` | 320,213 | 108,744 | 0 |
| This change alone | 338,482 (+5.7%) | 117,263 (+7.8%) | 0 |
| Coarse timing bias correction alone | 343,431 (+7.3%) | 119,470 (+9.9%) | 0 |
| Both | 344,522 (+7.6%) | 119,912 (+10.3%) | 0 |

So whichever of the two is merged second adds much less than its own
figures: this change on top of the bias correction adds 1,091 frames (+0.3%);
the bias correction on top of this change adds 6,040 (+1.8%).

## Results, tracking on

The 12 sets with both drift and clock offset (`_dc`), decoded with
`--tracking both` (the default configuration):

| Speed | Right frames, `feaec6f` | This branch | Whole messages, `feaec6f` | This branch | Wrong frames |
|---|---:|---:|---:|---:|---:|
| Normal | 24,361 | 24,772 (+1.7%) | 8,548 | 8,762 (+2.5%) | 0 → 0 |
| Fast | 18,848 | 20,448 (+8.5%) | 6,306 | 7,049 (+11.8%) | 0 → 0 |
| Turbo | 11,651 | 13,175 (+13.1%) | 3,509 | 4,156 (+18.4%) | 0 → 0 |
| Slow | 19,589 | 20,449 (+4.4%) | 6,552 | 6,919 (+5.6%) | 0 → 0 |
| **All** | **74,449** of 179,720 | **78,844 (+5.9%)** | **24,915** of 67,200 | **26,886 (+7.9%)** | **0 → 0** |

On the same 12 sets, tracking adds +1.7% right frames to this branch
(77,514 → 78,844).

## Processor time

Processor seconds per file (one file = one transmit period), one decoder
process at a time, 5 files per program start, pinned to one CPU, `feaec6f`
and both timing branches taking turns batch by batch. User-space instruction
counts (Linux `perf_event_open`) give the same ratios and do not depend on
other load on the machine. Tracking disabled:

| Set (files) | Speed | `feaec6f` | This branch | Ratio | Instructions ratio |
|---|---|---:|---:|---:|---:|
| 60 stations per file (30) | Normal | 0.225 s | 0.244 s | 1.08 | 1.08 |
| 37 stations per file (30) | Fast | 0.216 s | 0.247 s | 1.15 | 1.14 |
| 19 stations per file (30) | Turbo | 0.116 s | 0.127 s | 1.09 | 1.09 |
| 60 stations per file (30) | Slow | 0.736 s | 0.861 s | 1.17 | 1.12 |
| 4 stations per file (20) | Normal | 0.063 s | 0.064 s | 1.01 | 1.04 |
| Noise only (20) | Normal | 0.024 s | 0.023 s | 0.99 | 1.01 |

With `--tracking both` the ratios are 1.01 to 1.15. The largest share of one
core per period is crowded Slow with tracking on: 5.6% for this branch
against 4.9% for `feaec6f` (the test program decodes each file twice in that
mode, once without tracking; both runs are counted).

## The repository's own checks

- CTest, Debug and Release: 35 of 35 tests pass.
- Reference decode counts (`kReference` in `tools/wav_decode_test.cpp`),
  modes A and E: pass.
- Tracking checks on the 9 reference recordings (`--tracking both`, plain,
  with `--wrap` and with `--tracked-sic`, modes A and E): pass.
- Desktop comparison (`tools/reference/compare_wav_results.py`, 3 Hz,
  0.15 s, 6 dB, quality 0.30, against the desktop decoder at `0130e04`): all
  28 desktop records matched, no extra frames.

## Not tested

- Tracking on for the 36 sets without both drift and clock offset.
- Live on-air audio with this branch alone.
- Speed on a phone.
