#!/usr/bin/env python3
"""Summarize paired JS8Core soft-demapper benchmark CSVs."""

import argparse
import csv
from collections import defaultdict
from pathlib import Path


def read_run(path):
    run = {}
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            key = (row["mode"], row["case"], int(row["seed"]))
            if key in run:
                raise ValueError(f"{path}: duplicate trial {key}")
            run[key] = row
    return run


def summarize(legacy, soft):
    if not legacy or legacy.keys() != soft.keys():
        raise ValueError("runs must contain the same nonempty set of trials")

    def counts(rows):
        baseline, candidate, gained, lost, unexpected_a, unexpected_b = (0,) * 6
        cpu_a, cpu_b = 0.0, 0.0
        for before, after in rows:
            for frame in ("primary", "secondary"):
                a, b = int(before[frame]), int(after[frame])
                baseline += a
                candidate += b
                gained += b > a
                lost += a > b
            unexpected_a += int(before["unexpected"])
            unexpected_b += int(after["unexpected"])
            cpu_a += float(before["cpu_ms"])
            cpu_b += float(after["cpu_ms"])
        return (baseline, candidate, gained, lost, unexpected_a, unexpected_b, cpu_a, cpu_b)

    by_case = defaultdict(list)
    for key in sorted(legacy):
        before, after = legacy[key], soft[key]
        if any(before[field] != after[field] for field in ("input_hash", "expected")):
            raise ValueError(f"nonmatching inputs for trial {key}")
        pair = (before, after)
        mode, scenario, _ = key
        by_case[(mode, scenario)].append(pair)
        by_case[("all", scenario)].append(pair)
        by_case[("all", "all")].append(pair)

    print("mode,case,trials,legacy_correct,soft_correct,gained,lost,"
          "legacy_unexpected,soft_unexpected,legacy_cpu_ms,soft_cpu_ms")
    for (mode, scenario), rows in sorted(by_case.items()):
        *frames, cpu_a, cpu_b = counts(rows)
        print(",".join(map(str, (mode, scenario, len(rows), *frames, f"{cpu_a:.3f}", f"{cpu_b:.3f}"))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("legacy", type=Path)
    parser.add_argument("soft", type=Path)
    args = parser.parse_args()
    try:
        summarize(read_run(args.legacy), read_run(args.soft))
    except (ValueError, KeyError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
