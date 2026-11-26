# Coherent Costas acquisition

JS8Core uses the coherent Costas scorer in a default-enabled deep-sync fallback.
Set `EngineConfig::deep_sync` or `DecodeParams::deep_sync` to `false` to disable it.
The ordinary acquisition path, symbol tracking, and both cancellation paths
run first, across all requested submodes. Deep sync can append new CRC-valid
frames but cannot replace an earlier result or modify the audio.

## Score

For each trial start and frequency, match each complete seven-symbol Costas
block against a phase-continuous, unit-amplitude reference. The reference uses
the known JS8 tone sequence, and the trial frequency correction keeps phase
across symbol boundaries. The absolute phase is independent for each block:
intervening data and changes in propagation do not have to preserve it.

For a block with \(L\) observed samples \(x_n\) and reference samples \(r_n\),
including the trial frequency correction, the normalized squared correlation
is

\[
    q = \frac{|\sum_n x_n \overline{r_n}|^2}{L\sum_n |x_n|^2}.
\]

Each usable block gets a score from 0 to 1 regardless of absolute signal
level. The scorer returns the individual block scores and uses the
second-best score for cross-block support. A single strong match cannot
outvote the other two blocks. No score is returned unless two blocks are
complete and have finite, nonzero energy. Use the actual observed span,
not zero-padded samples or nominal payload length, when calling the scorer.

This is a ranking statistic, not a detection probability. Selecting the
largest score over many trials raises its noise floor. The scorer alone
sets no acceptance threshold and does not report frames.

## Bounded fallback

Each submode rescans the final residual once and keeps at most eight separated
coarse seeds, including peaks below the normal coarse threshold. Each seed
gets a local search within half a symbol and half a tone spacing of the seed,
clipped to the configured band. There are at most 135 coarse cells and 35
refinement cells per seed. Admit peaks with at least 0.16 cross-block support,
rank them by support, and try at most three distinct candidates per submode.
The demodulator uses the winning time and frequency and the actual observed
window. It runs BP passes 1–4 using the existing sync, hard-error, and CRC
checks, with no additional BP rescue, OSD, or interference subtraction.

The fallback shares the engine's `optional_decode_budget_ms`; it checks
permission during the scan, during coherent search, between decode stages,
and before reporting a frame. Denial discards partial searches and preserves
ordinary results. The score threshold and search caps have been checked on
synthetic and pinned WAV fixtures; they do not establish an on-air false-decode
rate or low-power CPU budget.

## Tests

`js8core-coherent-costas-test` uses an independently synthesized baseband
waveform. It covers all five submode geometries, block-to-block phase and
gain changes, frequency sign and timing alignment, clipped captures,
incorrect signals such as a strong continuous tone, and a small two-dimensional
search in noise. It checks the search-cell cap and denies work during coarse
search, refinement, and the final permission check. The receive-path regression
appends a late Turbo frame with the expected timing and frequency, preserves
ordinary frames across submodes, and covers policy denial, ring wrap and
truncated input. A second Turbo fixture attenuates the pilots but not the
data: the fallback recovers it, while ordinary acquisition and a fallback
restricted to the ordinary coarse cutoff both miss it. A noise-only window
and a strong carrier with noise produce no frames in the receive-path
regression.

Pass `--deep-sync` to `js8core-wav-decode-test` to enable the fallback for
comparison against the established path on the same WAV; it checks that existing records
and tracked cancellation counts are unchanged before appending any new frames.
With the fallback enabled, the pinned waveform check matched all 28 desktop
records and both expected JS8Core-only records, with no additional outputs.
