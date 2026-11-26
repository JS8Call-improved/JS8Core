#!/usr/bin/env python3
"""Compare independently built JS8Core and pinned desktop JS8Call WAV decodes.

The external desktop source is compiled only in a test runner; the cleanroom
library never links or distributes it. Per-message identity is exact. Numeric
DSP quantities have explicit, conservative tolerances rather than exact
floating-point equality across two FFT implementations.
Known JS8Core-only recoveries may be supplied as additional required records.
"""
import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Decode:
    file: str
    kind: int
    mode: int
    snr: int
    frequency_hz: float
    xdt_seconds: float
    quality: float
    payload_hex: str

    @property
    def key(self):
        return (self.file, self.kind, self.mode, self.payload_hex)

    def describe(self):
        try:
            msg = bytes.fromhex(self.payload_hex).decode("utf-8")
        except UnicodeError:
            msg = repr(bytes.fromhex(self.payload_hex))
        return (
            f"{self.file} [{self.kind}/{self.mode}] {msg!r}"
            f" @ {self.frequency_hz:.2f} Hz, {self.xdt_seconds:.3f} s,"
            f" {self.snr} dB, quality={self.quality:.3f}"
        )


def load(path: Path):
    records = []
    for line_no, line in enumerate(path.read_text().splitlines(), 1):
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != 9 or fields[0] != "RECORD":
            raise ValueError(f"{path}:{line_no}: malformed decode record: {line!r}")
        _, filename, kind, mode, snr, hz, dt, quality, payload = fields
        if len(payload) % 2:
            raise ValueError(f"{path}:{line_no}: odd hex length")
        bytes.fromhex(payload)
        records.append(
            Decode(filename, int(kind), int(mode), int(snr), float(hz),
                   float(dt), float(quality), payload)
        )
    return records


def compare(refs, candidates, freq_tolerance, dt_tolerance, snr_tolerance, quality_tolerance,
            *, expected_core_only=()):
    reference = defaultdict(list)
    actual = defaultdict(list)
    for record in refs:
        reference[record.key].append(("desktop", record))
    desktop_keys = set(reference)
    for record in expected_core_only:
        if record.key in desktop_keys:
            print(f"FAIL: expected JS8Core-only record is also in desktop output: {record.describe()}")
            return True
        reference[record.key].append(("JS8Core-only", record))
    for record in candidates:
        actual[record.key].append(record)

    failed = 0
    matched = Counter()
    desktop_counts = Counter(record.file for record in refs)
    core_only_counts = Counter(record.file for record in expected_core_only)
    core_counts = Counter(record.file for record in candidates)
    files = sorted(set(desktop_counts) | set(core_only_counts) | set(core_counts))
    for name in files:
        r_count, e_count, a_count = desktop_counts[name], core_only_counts[name], core_counts[name]
        if r_count + e_count != a_count:
            print(f"COUNT DIFF {name}: desktop={r_count} expected-core-only={e_count} js8core={a_count}")
            failed += 1
        elif e_count:
            print(f"COUNT OK   {name}: {a_count} decodes ({r_count} desktop + {e_count} expected core-only)")
        else:
            print(f"COUNT OK   {name}: {r_count} decodes")

    # Match by exact (file, frame type, mode, payload bytes), but tolerate
    # candidate enumeration order differences across implementations.
    for key in sorted(set(reference) | set(actual)):
        expected = reference[key]
        available = list(actual[key])
        for source, record in expected:
            if not available:
                print(f"MISSING JS8CORE ({source}) {record.describe()}")
                failed += 1
                continue
            def distance(other):
                return (abs(other.frequency_hz - record.frequency_hz)
                        + 10 * abs(other.xdt_seconds - record.xdt_seconds)
                        + 0.25 * abs(other.snr - record.snr))
            index = min(range(len(available)), key=lambda j: distance(available[j]))
            core = available.pop(index)
            deltas = {
                "frequency": (abs(core.frequency_hz - record.frequency_hz), freq_tolerance),
                "timing": (abs(core.xdt_seconds - record.xdt_seconds), dt_tolerance),
                "SNR": (abs(core.snr - record.snr), snr_tolerance),
                "quality": (abs(core.quality - record.quality), quality_tolerance),
            }
            out_of_range = {name: (delta, allowed) for name, (delta, allowed) in deltas.items()
                            if delta > allowed}
            if out_of_range:
                print(f"METADATA DIFF ({source}) {record.describe()}")
                print(f"       JS8CORE {core.describe()}")
                print(f"       DELTAS  {out_of_range}")
                failed += 1
            else:
                matched[source] += 1
        for extra in available:
            print(f"EXTRA JS8CORE {extra.describe()}")
            failed += 1

    print(f"\nExact identity/metadata matches: {matched['desktop']}/{len(refs)} desktop records")
    if expected_core_only:
        print(f"Expected JS8Core-only matches: {matched['JS8Core-only']}/{len(expected_core_only)}")
    if failed:
        print(f"FAIL: {failed} desktop interoperability mismatches")
    else:
        print(f"PASS: {len(files)} WAV files and all {sum(matched.values())} expected records matched")
    return bool(failed)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--desktop", type=Path, required=True)
    cli.add_argument("--core", type=Path, required=True)
    cli.add_argument("--expected-core-only", type=Path,
                     help="TSV of required JS8Core records absent from the pinned desktop output")
    cli.add_argument("--freq-hz", type=float, default=3.0)
    cli.add_argument("--dt-seconds", type=float, default=0.15)
    cli.add_argument("--snr-db", type=float, default=6.0)
    cli.add_argument("--quality", type=float, default=0.30)
    options = cli.parse_args()
    expected_core_only = load(options.expected_core_only) if options.expected_core_only else ()
    raise SystemExit(int(compare(load(options.desktop), load(options.core),
                                 options.freq_hz, options.dt_seconds,
                                 options.snr_db, options.quality,
                                 expected_core_only=expected_core_only)))


if __name__ == "__main__":
    main()
