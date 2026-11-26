#!/usr/bin/env python3
"""Exercise the WAV recorder's paired mode and input boundaries without RF data."""

import argparse
import math
from pathlib import Path
import struct
import subprocess
import tempfile


def chunk(kind, data):
    return kind + struct.pack("<I", len(data)) + data + (b"\0" if len(data) % 2 else b"")


def riff(*chunks):
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    checks = 0

    def run(options, expected=0):
        nonlocal checks
        result = subprocess.run(
            [str(args.executable), *map(str, options)],
            capture_output=True, text=True, timeout=30, check=False,
        )
        checks += 1
        if any(marker in result.stderr for marker in
               ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")):
            raise AssertionError(f"sanitizer diagnostic for {options}\n{result.stderr}")
        if result.returncode != expected:
            raise AssertionError(
                f"{options}: expected exit {expected}, got {result.returncode}\n"
                f"stdout: {result.stdout}\nstderr: {result.stderr}"
            )
        return result

    pcm = struct.pack("<HHIIHH", 1, 1, 12000, 24000, 2, 16)
    fmt = chunk(b"fmt ", pcm)
    with tempfile.TemporaryDirectory(prefix="wav-tracking-", dir=args.work_dir) as directory:
        root = Path(directory)
        silence = root / "silence.wav"
        silence.write_bytes(riff(fmt, chunk(b"data", bytes(30 * 12000 * 2))))
        for mode in ("A", "E"):
            default = run(["--records", mode, silence])
            if default.stdout or "PAIR\t" in default.stderr:
                raise AssertionError("default record output changed or silence decoded")
            for tracking in ("disabled", "frequency", "timing", "both"):
                result = run(["--records", "--tracking", tracking, mode, silence])
                if result.stdout:
                    raise AssertionError("silence emitted a frame record")
                pairs = [line.split("\t") for line in result.stderr.splitlines()
                         if line.startswith("PAIR\t")]
                if tracking == "disabled":
                    if pairs:
                        raise AssertionError("disabled mode performed a paired run")
                elif len(pairs) != 1 or len(pairs[0]) != 7 or pairs[0][2:5] != ["0", "0", "0"]:
                    raise AssertionError(f"invalid paired summary: {pairs}")
                elif any(not math.isfinite(float(value)) or float(value) < 0
                         for value in pairs[0][5:]):
                    raise AssertionError("invalid decode timing")
            run(["--tracking", "both", "--wrap", mode, silence])
            result = run(["--tracking", "both", "--tracked-sic", "--wrap", mode, silence])
            if f"SIC\t{silence.name}\t0" not in result.stderr:
                raise AssertionError("silence incorrectly reached tracked cancellation")

        # An odd-sized unknown RIFF chunk needs a pad byte before the next header.
        padded = root / "padded.wav"
        padded.write_bytes(riff(chunk(b"JUNK", b"abc"), fmt, chunk(b"data", bytes(24000))))
        run(["--tracking", "both", "A", padded])
        short = root / "short.wav"
        short.write_bytes(riff(fmt, chunk(b"data", b"\0\0")))
        run(["--tracking", "both", "A", short])

        bad_files = {
            "odd-pcm": riff(fmt, chunk(b"data", b"abc")),
            "empty-data": riff(fmt, chunk(b"data", b"")),
            "short-fmt": riff(chunk(b"fmt ", pcm[:14]), chunk(b"data", b"\0\0")),
            "missing-fmt": riff(chunk(b"data", b"\0\0")),
            "missing-data": riff(fmt),
            "compressed": riff(chunk(b"fmt ", struct.pack("<HHIIHH", 3, 1, 12000, 24000, 2, 16)),
                               chunk(b"data", b"\0\0")),
            "stereo": riff(chunk(b"fmt ", struct.pack("<HHIIHH", 1, 2, 12000, 48000, 4, 16)),
                           chunk(b"data", bytes(4))),
            "wrong-rate": riff(chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, 48000, 96000, 2, 16)),
                               chunk(b"data", b"\0\0")),
            "wrong-width": riff(chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, 12000, 12000, 1, 8)),
                                chunk(b"data", bytes(2))),
            "wrong-alignment": riff(chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, 12000, 24000, 4, 16)),
                                    chunk(b"data", bytes(4))),
            "wrong-byte-rate": riff(chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, 12000, 1, 2, 16)),
                                    chunk(b"data", bytes(2))),
            "oversized-chunk": riff(fmt, b"data" + struct.pack("<I", 100) + bytes(2)),
            "unbounded-riff": b"RIFF" + struct.pack("<I", 0xffffffff) + b"WAVE",
            "missing-pad": riff(b"JUNK" + struct.pack("<I", 3) + b"abc", fmt,
                                chunk(b"data", bytes(2))),
            "too-long": riff(fmt, chunk(b"data", bytes(2 * (60 * 12000 + 1)))),
            "truncated": riff(fmt, chunk(b"data", bytes(100)))[:-1],
        }
        for name, contents in bad_files.items():
            path = root / (name + ".wav")
            path.write_bytes(contents)
            result = run(["--tracking", "both", "A", path], expected=1)
            if "cannot read" not in result.stderr or "PAIR\t" in result.stderr:
                raise AssertionError(f"invalid WAV was not rejected before decoding: {name}")

        # Frozen fixture count gates must still use the disabled baseline, even
        # when paired tracking is requested. Record mode keeps its old contract
        # of delegating fixture identity/count checks to the external comparator.
        known = root / "A_1_4.wav"
        known.write_bytes(silence.read_bytes())
        result = run(["--tracking", "both", "A", known], expected=1)
        if "expected reference 5" not in result.stderr:
            raise AssertionError("tracking bypassed the frozen baseline count gate")
        run(["--tracking", "both", "--records", "A", known])
        for options in ([], ["--records"], ["--tracking"],
                        ["--tracking", "bad", "A", silence],
                        ["--unknown", "A", silence], ["B", silence], ["A"]):
            run(options, expected=2)
    print(f"PASS: {checks} WAV recorder CLI/input checks")


if __name__ == "__main__":
    main()
