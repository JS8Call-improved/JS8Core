# Noncoherent soft symbol likelihoods

JS8Core uses noncoherent soft symbol likelihoods by default in **ordinary
pass 2**, without adding a BP attempt. Pass 1, the erasure and rescue passes,
tracked demodulation, and OSD's input and acceptance rules are unchanged. If
the soft metric cannot be formed, pass 2 falls back to the logarithmic metric.
The logarithmic metric is only calculated when pass 2 uses it. Build with
`-DJS8CORE_SOFT_DEMAPPER=OFF` to use that metric for comparison.

## Model

The existing eight tone-bin values are FFT magnitudes, not energies. Square
them before estimating tone likelihoods. In the model, a noise-only complex bin
has circular Gaussian variance \(N = E[|Z|^2]\). Under tone hypothesis \(k\),
bin \(k\) has variance \(N+S\), from marginalizing a zero-mean circular
Gaussian prior on the unknown complex signal amplitude, while the other bins
remain noise-only. The quantity that differs among tone hypotheses is

\[
    s_k = |Z_k|^2\,\frac{S}{N+S}\,\frac{1}{N}.
\]

The Gaussian determinant penalty is common to the eight hypotheses. For each
bit, subtract the log-sum-exp of scores for bit zero from the log-sum-exp for
bit one, with equal tone priors. This uses all four tones on each side, unlike
max-of-four. It does not require a phase estimate. The model assumes independent
bins with a common noise floor; leakage, collisions, and fading can violate
that assumption.

Estimate \(N\) from the median inactive-bin energy divided by \(\ln 2\), since
a noise-only complex Gaussian has exponential energy. The known Costas tone
identifies inactive bins without selecting only small observed values. Estimate
\(N+S\) the same way from the active bins, using the Gaussian amplitude model
as an approximation. Require at least 14 complete Costas symbols and an active
estimate larger than the noise estimate. Invalid estimates fall back to the
legacy metric. The resulting LLRs use unit model scale with no legacy
per-frame normalization and are capped at ±12 before BP. These are initial
engineering choices, not calibration constants taken from another decoder.
Data symbols that aren't completely inside the observed window are erasures
(three zero LLRs), even if the FFT assigned them nonzero magnitudes.

## Reproduce the paired experiment

```sh
cmake -S . -B build-legacy -DCMAKE_BUILD_TYPE=Release -DJS8CORE_SOFT_DEMAPPER=OFF
cmake -S . -B build-soft -DCMAKE_BUILD_TYPE=Release
cmake --build build-legacy --target js8core-soft-likelihood-benchmark js8core-soft-likelihood-test
cmake --build build-soft --target js8core-soft-likelihood-benchmark js8core-soft-likelihood-test
ctest --test-dir build-legacy -R '^js8core-soft-likelihood$' --output-on-failure
ctest --test-dir build-soft -R '^js8core-soft-likelihood$' --output-on-failure
build-legacy/js8core-soft-likelihood-benchmark 1000 12 > legacy.csv
build-soft/js8core-soft-likelihood-benchmark 1000 12 > soft.csv
python3 tools/compare_soft_likelihood_runs.py legacy.csv soft.csv
```

Use low seeds for development and disjoint seeds for a held-out comparison.
Each run uses all five modes and synthesizes weak single frames, strong
single frames, strong overlapping frames, and noise-only windows. The dB
levels in the case labels are nominal matched-bin SNRs, not on-air SNRs in
a reference bandwidth. The comparator rejects unpaired inputs and reports
individual frames gained and lost, unexpected decodes, and process CPU time.
Compare builds on the same host and standard library: the benchmark hashes
the input samples to detect differences in synthesis.

The CPU figures exclude waveform synthesis and initial allocation, but
include the full decoder, including any change in subsequent work after
pass 2. Tracking and deep sync are off and the offline OSD fallback remains available.
These trials do not measure streaming queue pressure or establish an on-air
false-decode rate. Use the pinned WAV parity tests alongside the synthetic
benchmark when changing the demapper.

The regression suite also isolates a Turbo waveform with strong pilots and
attenuated, noisy data. With tracking and OSD disabled, the default pass 2
recovers the frame; the legacy pass-2 build does not. Unit tests check that
partial leading and trailing data symbols contribute no bit evidence.

## Initial results

The initial development smoke run (seeds 0–1) recovered the same 48 frames
in both builds. The held-out runs recovered the same frames in both builds:

| Seeds | Paired windows | Correct frames in each build | Gained / lost | Unexpected in either build |
|---|---:|---:|---:|---:|
| 1000–1011 | 360 | 265 | 0 / 0 | 0 |
| 2000–2047 | 1,440 | 1,112 | 0 / 0 | 0 |

Across both runs, both builds recovered all 300 strong frames and all 600
frames from strong-collision windows. On the larger run, decoder process
CPU was 18.28 s for legacy and 18.46 s for soft on the same arm64 Mac; a
single comparison doesn't establish a performance difference. Both builds
matched 28/28 pinned desktop records and 2/2 expected JS8Core-only records.

The development and held-out runs used the same initial scale, without
retuning. They establish a starting baseline, not a sensitivity improvement.
Further validation could separate the demapper from acquisition and the
existing recovery passes, then test phase or fading impairments on new
waveforms.
