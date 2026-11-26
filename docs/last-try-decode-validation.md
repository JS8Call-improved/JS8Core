# Last-try decode: validation

One change in `src/decoder/ldpc.cpp` and `src/decoder/legacy_decoder.cpp`:
when belief propagation fails on a candidate, the decoder makes one last try
with ordered statistics decoding (OSD) on the full-evidence reading, and keeps
the answer only if it passes four checks besides the CRC. The candidate search,
timing, the reading of the tones and passes 1 to 5 are unchanged, and tracked
attempts (`--tracking`) do not use the last try.

In the engine, the last try now shares `EngineConfig::optional_decode_budget_ms`
with tracking and tracked cancellation. It yields to queued captures or an
expired deadline; a search interrupted by either condition cannot report a
frame. OSD results are report-only: they never subtract from the audio or
trigger another cancellation pass. This prevents a CRC-valid false positive
from masking other signals, but also means that an OSD-only recovery cannot
uncover a weaker signal by cancellation. A frame independently recovered by
belief propagation is still eligible for subtraction.

The historical measurements below used the original offline implementation,
which allowed OSD subtraction, compared nearby frames by text alone, and did
not apply the shared hard-error limits or an engine optional-work deadline to
OSD.

## What the last try does

1. **Search order.** Three damped belief-propagation rounds on the same
   evidence (check messages times 0.6) give each bit a refined value. The
   87 most reliable independent positions by that value are taken as the
   information set (Fossorier, IEEE JSAC 19(5), 2001; Zhang, Yue, Li, Vucetic,
   arXiv 2306.00443, 2023: floor(girth / 4) + 1 = 3 rounds for this code's
   girth of 8).
2. **Search.** Every codeword with up to 2 flips on those positions is
   scored, and 3 flips where the Wu and Hadjicostis preprocessing rule allows
   it (IEEE Trans. Inf. Theory 53(1), 2007, Theorem 2). The score is the sum
   of |LLR| over bits that disagree with the hard decisions of the evidence.
3. **Checks before the CRC**, because OSD always returns its closest
   codeword:
   - at least 9 of the 21 Costas tones match (the same minimum as pass 5);
   - the best score is at most 6.5% of the total |LLR|;
   - the best score beats the runner-up by at least 0.25% of the total |LLR|
     (Forney's test, approximated with the runner-up);
   - the shared late-pass limit allows at most 39 hard-decision disagreements,
     or 35 when the sync score is below 2.0, in addition to the weighted limits.
4. **Checks after the CRC:**
   - the frame must be readable by one of the unpackers (fast data when type
     bit 4 is set, else data, heartbeat, compound or directed); an unreadable
     frame would be shown as its 12 raw characters. Only last-try answers
     are checked. The fork checks every pass, but a refused frame is not
     subtracted either: on test audio with random frame text, the frames
     under it were then lost (Slow crowded set: 841 → 724 right frames with
     every pass checked, 841 → 1,007 with only the last try checked);
   - a last-try answer is refused when a frame with a different type or text
     was already reported within 3 Hz and 0.05 s of it in this period, unless
     an exact type-and-text match was also reported within that window. A
     conflicting answer may be a subtraction residual misread as a new frame.

Each last-try answer that reaches the CRC check has about a 1 in 4,096 chance
of being a wrong frame that passes it (12-bit CRC). The checks above set how
many reach it.

These changes were first made and measured in a fork
(https://github.com/RobBobbins/JS8Core, commits `6a85ca0`, `02d757d`,
`e186de1`, `7231c03`, `02e9174`, `f173817`, `a962669`, `6012b1e` and
`149b046`). This branch carries only them, on top of `feaec6f`, without
the fork's other passes.

## Test audio

48 sets of computer-made JS8 audio, the same as for #7 and #8
(`docs/coarse-timing-bias-validation.md` and
`docs/second-fine-timing-validation.md` on those branches). One list of 700 messages
(100 each of heartbeat, CQ, contact reply, directed short text, relay,
ragchew and weather report, from 40 stations) is built into frames by
JS8Core's `build_message_frames` and sent at all four speeds and 8 levels
(-6 to -27 dB in 3 dB steps). A message of several frames goes out in
consecutive cycles at one frequency and start time, like a station sending
it.

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
| Normal | 98,057 | 103,566 (+5.6%) | 34,450 | 36,755 (+6.7%) | 0 → 1 |
| Fast | 75,672 | 81,962 (+8.3%) | 25,163 | 27,700 (+10.1%) | 0 → 0 |
| Turbo | 47,191 | 54,765 (+16.0%) | 14,403 | 17,168 (+19.2%) | 0 → 0 |
| Slow | 99,293 | 103,843 (+4.6%) | 34,728 | 36,530 (+5.2%) | 0 → 1 |
| **All** | **320,213** of 718,880 | **344,136 (+7.5%)** | **108,744** of 268,800 | **118,153 (+8.7%)** | **0 → 2** |

- By kind, all speeds: no fading +3.7% right frames, fading +11.7%, crowded
  +8.3%.
- Frames gained and lost: 23,934 right frames decoded only by this branch;
  11 decoded only by `feaec6f` (Fast 7, Turbo 2, Slow 2).
- The two wrong frames, both new with this branch:
  - `pz5x3LDlSJm0`: crowded Normal set with drift and clock offset, reported
    at -29 dB and 2214.1 Hz; reads as a data frame "O ".
  - `NXo3T0mxjvDX`: fading Slow set, reported at -36 dB and 1010.9 Hz; reads
    as a compound frame "LYY/OJN/IJK".

  That is 1 wrong frame per 172,068 right frames. In the fork, 33.5% of
  random frames were unreadable, so the readable-frame check refuses about
  a third of wrong frames that pass the CRC.

## Results, tracking on

The 12 sets with both drift and clock offset (`_dc`), decoded with
`--tracking both` (the default configuration):

| Speed | Right frames, `feaec6f` | This branch | Whole messages, `feaec6f` | This branch | Wrong frames |
|---|---:|---:|---:|---:|---:|
| Normal | 24,361 | 25,773 (+5.8%) | 8,548 | 9,139 (+6.9%) | 0 → 1 |
| Fast | 18,848 | 20,347 (+8.0%) | 6,306 | 6,922 (+9.8%) | 0 → 0 |
| Turbo | 11,651 | 13,253 (+13.7%) | 3,509 | 4,097 (+16.8%) | 0 → 0 |
| Slow | 19,589 | 20,524 (+4.8%) | 6,552 | 6,892 (+5.2%) | 0 → 0 |
| **All** | **74,449** of 179,720 | **79,897 (+7.3%)** | **24,915** of 67,200 | **27,050 (+8.6%)** | **0 → 1** |

The wrong frame is `pz5x3LDlSJm0`, the same as with tracking disabled. On
these 12 sets, tracking adds +2.5% right frames to `feaec6f` (72,602 →
74,449) and +1.1% to this branch (79,003 → 79,897).

## Processor time

User-space instruction counts per file (Linux `perf_event_open`), one decoder
process at a time, 5 files per program start, pinned to one CPU, the decoders
taking turns batch by batch. Instruction counts do not depend on other load
on the machine; processor seconds do, and other test runs shared the machine
during these measurements, so the seconds are given only as a guide. These
sets are older computer-made sets with random frame text (not the 48 sets
above); tracking disabled:

| Set (files) | Speed | Instructions, `feaec6f` | This branch | Ratio | Processor s, `feaec6f` → branch |
|---|---|---:|---:|---:|---:|
| 60 stations per file (25) | Normal | 2.61 G | 3.42 G | 1.31 | 0.455 → 0.547 |
| 37 stations per file (30) | Fast | 2.38 G | 2.77 G | 1.16 | 0.230 → 0.278 |
| 19 stations per file (30) | Turbo | 1.27 G | 1.51 G | 1.19 | 0.127 → 0.152 |
| 60 stations per file (30) | Slow | 7.16 G | 8.29 G | 1.16 | 0.857 → 0.993 |
| 4 stations per file (20) | Normal | 0.62 G | 0.62 G | 1.01 | 0.063 → 0.064 |
| Noise only (20) | Normal | 0.23 G | 0.24 G | 1.00 | 0.028 → 0.026 |

With `--tracking both` the instruction ratios are 1.00 to 1.30. The cost is
in crowded bands; on a quiet band and on noise it is about zero.

On the same files, right frames: Normal 116 → 190, Fast 242 → 310, Turbo
95 → 146 (1 wrong frame with this branch), Slow 841 → 1,007; no right frame
that `feaec6f` decodes is lost on any of the four crowded sets.

## The repository's own checks

- CTest, Debug and Release: 35 of 35 tests pass.
- Tracking checks on the 9 reference recordings (`--tracking both`, with
  `--wrap` and `--tracked-sic`): no changed or invalid results. The mode A
  runs still fail, on the reference count below.
- Reference decode counts (`kReference` in `tools/wav_decode_test.cpp`) and
  the desktop comparison (`tools/reference/compare_wav_results.py`): all 28
  desktop frames are decoded with matching metadata, and one more:
  `SJWkJnSNwzqH` in `A_1_4.wav` (805.75 Hz, 0.220 s, -11 dB). `A_2_5.wav` is
  the same recording (same MD5), so both checks count it twice: A_1_4 and
  A_2_5 give 5 decodes where 4 are expected.
- That frame reads as `KD8SKZ: KN4CRD HEARTBEAT SNR -14`. In the same
  recording `KG9B`, `VA3QR` and `K0OG` answer `KN4CRD` the same way. The
  JS8Call-improved v3.0.3 release decodes it too; the desktop reference
  (`0130e04`) and `feaec6f` do not.

## Not tested

- Tracking on for the 36 sets without both drift and clock offset.
- Live on-air audio with this branch alone (the fork's live comparison runs
  its full decoder, which includes this last try).
- Speed on a phone.
