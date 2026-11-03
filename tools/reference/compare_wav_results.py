#!/usr/bin/env python3
"""Compare independently built JS8Core and pinned desktop JS8Call WAV decodes.

The external desktop source is compiled only in a test runner; the cleanroom
library never links or distributes it. Per-message identity is exact. Numeric
DSP quantities have explicit, conservative tolerances rather than exact
floating-point equality across two FFT implementations.
"""
import argparse
from collections import defaultdict
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


def compare(refs, candidates, freq_tolerance, dt_tolerance, snr_tolerance, quality_tolerance):
    reference = defaultdict(list)
    actual = defaultdict(list)
    for record in refs:
        reference[record.key].append(record)
    for record in candidates:
        actual[record.key].append(record)

    failed = 0
    matched = 0
    files = sorted({r.file for r in refs + candidates})
    for name in files:
        r_count = sum(1 for r in refs if r.file == name)
        a_count = sum(1 for r in candidates if r.file == name)
        if r_count != a_count:
            print(f"COUNT DIFF {name}: desktop={r_count} js8core={a_count}")
            failed += 1
        else:
            print(f"COUNT OK   {name}: {r_count} decodes")

    # Match by exact (file, frame type, mode, payload bytes), but tolerate
    # candidate enumeration order differences across implementations.
    for key in sorted(set(reference) | set(actual)):
        expected = reference[key]
        available = list(actual[key])
        for desktop in expected:
            if not available:
                print(f"MISSING JS8CORE {desktop.describe()}")
                failed += 1
                continue
            def distance(other):
                return (abs(other.frequency_hz - desktop.frequency_hz)
                        + 10 * abs(other.xdt_seconds - desktop.xdt_seconds)
                        + 0.25 * abs(other.snr - desktop.snr))
            index = min(range(len(available)), key=lambda j: distance(available[j]))
            core = available.pop(index)
            deltas = {
                "frequency": (abs(core.frequency_hz - desktop.frequency_hz), freq_tolerance),
                "timing": (abs(core.xdt_seconds - desktop.xdt_seconds), dt_tolerance),
                "SNR": (abs(core.snr - desktop.snr), snr_tolerance),
                "quality": (abs(core.quality - desktop.quality), quality_tolerance),
            }
            out_of_range = {name: (delta, allowed) for name, (delta, allowed) in deltas.items()
                            if delta > allowed}
            if out_of_range:
                print(f"METADATA DIFF {desktop.describe()}")
                print(f"       JS8CORE {core.describe()}")
                print(f"       DELTAS  {out_of_range}")
                failed += 1
            else:
                matched += 1
        for extra in available:
            print(f"EXTRA JS8CORE {extra.describe()}")
            failed += 1

    print(f"\nExact identity/metadata matches: {matched}/{len(refs)} desktop records")
    if failed:
        print(f"FAIL: {failed} desktop interoperability mismatches")
    else:
        print(f"PASS: {len(files)} WAV files and all {matched} decoded records matched")
    return bool(failed)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--desktop", type=Path, required=True)
    cli.add_argument("--core", type=Path, required=True)
    cli.add_argument("--freq-hz", type=float, default=3.0)
    cli.add_argument("--dt-seconds", type=float, default=0.15)
    cli.add_argument("--snr-db", type=float, default=6.0)
    cli.add_argument("--quality", type=float, default=0.30)
    options = cli.parse_args()
    raise SystemExit(int(compare(load(options.desktop), load(options.core),
                                 options.freq_hz, options.dt_seconds,
                                 options.snr_db, options.quality)))


if __name__ == "__main__":
    main()
