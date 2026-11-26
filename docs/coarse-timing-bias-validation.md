# Coarse timing bias correction: validation

One change in `src/decoder/legacy_decoder.cpp`, in `find_candidates`: the
start time given to each candidate. The waveform, the fine timing and
frequency search, the LDPC decoding and the CRC acceptance rule are
unchanged.

The candidate spectra fill the whole 2-symbol FFT span under a window that
peaks one symbol in, so a tone scores best when the spectrum starts 2 coarse
steps (half a symbol) before it. Candidate times were taken from the window
start plus half a step, so they came out 1.5 coarse steps early:

| Speed | Bias removed |
|---|---:|
| Normal | -0.043 s |
| Fast | -0.039 s |
| Turbo | -0.020 s |
| Slow | -0.107 s |

The candidate time now counts from the true step:
`(best_offset + start_step + 2) * step_seconds - start_seconds`.

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
| Normal | 98,057 | 99,572 (+1.6%) | 34,450 | 35,219 (+2.2%) | 0 → 0 |
| Fast | 75,672 | 84,740 (+12.0%) | 25,163 | 29,312 (+16.5%) | 0 → 0 |
| Turbo | 47,191 | 56,290 (+19.3%) | 14,403 | 18,349 (+27.4%) | 0 → 0 |
| Slow | 99,293 | 102,829 (+3.6%) | 34,728 | 36,590 (+5.4%) | 0 → 0 |
| **All** | **320,213** of 718,880 | **343,431 (+7.3%)** | **108,744** of 268,800 | **119,470 (+9.9%)** | **0 → 0** |

- By kind, all speeds: no fading +4.4% right frames, fading +7.6%, crowded
  +10.5%.
- Frames gained and lost: 25,468 right frames decoded only by this branch;
  2,250 decoded only by `feaec6f` (Normal 371, Fast 439, Turbo 445, Slow 995).

## Overlap with the second fine timing search

The second fine timing search (a separate pull request) recovers mostly the
same frames. Same 48 sets, tracking disabled:

| Build | Right frames | Whole messages | Wrong frames |
|---|---:|---:|---:|
| `feaec6f` | 320,213 | 108,744 | 0 |
| This change alone | 343,431 (+7.3%) | 119,470 (+9.9%) | 0 |
| Second fine timing search alone | 338,482 (+5.7%) | 117,263 (+7.8%) | 0 |
| Both | 344,522 (+7.6%) | 119,912 (+10.3%) | 0 |

So whichever of the two is merged second adds much less than its own
figures: this change on top of the second search adds 6,040 frames (+1.8%);
the second search on top of this change adds 1,091 (+0.3%).

## Results, tracking on

The 12 sets with both drift and clock offset (`_dc`), decoded with
`--tracking both` (the default configuration):

| Speed | Right frames, `feaec6f` | This branch | Whole messages, `feaec6f` | This branch | Wrong frames |
|---|---:|---:|---:|---:|---:|
| Normal | 24,361 | 24,708 (+1.4%) | 8,548 | 8,712 (+1.9%) | 0 → 0 |
| Fast | 18,848 | 21,046 (+11.7%) | 6,306 | 7,299 (+15.7%) | 0 → 0 |
| Turbo | 11,651 | 13,984 (+20.0%) | 3,509 | 4,522 (+28.9%) | 0 → 0 |
| Slow | 19,589 | 20,446 (+4.4%) | 6,552 | 6,948 (+6.0%) | 0 → 0 |
| **All** | **74,449** of 179,720 | **80,184 (+7.7%)** | **24,915** of 67,200 | **27,481 (+10.3%)** | **0 → 0** |

On the same 12 sets, tracking adds +1.7% right frames to this branch
(78,881 → 80,184).

## Processor time

Processor seconds per file (one file = one transmit period), one decoder
process at a time, 5 files per program start, pinned to one CPU, `feaec6f`
and both timing branches taking turns batch by batch. User-space instruction
counts (Linux `perf_event_open`) give the same ratios and do not depend on
other load on the machine. Tracking disabled:

| Set (files) | Speed | `feaec6f` | This branch | Ratio | Instructions ratio |
|---|---|---:|---:|---:|---:|
| 60 stations per file (30) | Normal | 0.225 s | 0.236 s | 1.05 | 1.05 |
| 37 stations per file (30) | Fast | 0.216 s | 0.261 s | 1.21 | 1.19 |
| 19 stations per file (30) | Turbo | 0.116 s | 0.123 s | 1.06 | 1.06 |
| 60 stations per file (30) | Slow | 0.736 s | 0.853 s | 1.16 | 1.12 |
| 4 stations per file (20) | Normal | 0.063 s | 0.063 s | 1.01 | 1.02 |
| Noise only (20) | Normal | 0.024 s | 0.023 s | 0.99 | 1.00 |

With `--tracking both` the ratios are 0.99 to 1.20. The largest share of one
core per period is crowded Slow with tracking on: 5.7% for this branch
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
