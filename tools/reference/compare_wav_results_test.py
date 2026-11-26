#!/usr/bin/env python3
"""Regression tests for desktop parity plus known JS8Core-only WAV decodes."""
from contextlib import redirect_stdout
from dataclasses import replace
from io import StringIO
import unittest

from compare_wav_results import Decode, compare


class CompareWavResultsTest(unittest.TestCase):
    desktop = Decode("example.wav", 3, 0, -15, 521.375, 0.140, 1.0, "4142")
    core_only = Decode("example.wav", 3, 0, -11, 805.750, 0.220, 0.7, "4344")

    def compare(self, records, expected):
        with redirect_stdout(StringIO()) as output:
            failed = compare([self.desktop], records, 3, 0.15, 6, 0.30,
                             expected_core_only=expected)
        return failed, output.getvalue()

    def test_required_core_only_record_matches_within_tolerance(self):
        shifted = replace(self.core_only, frequency_hz=806.0, xdt_seconds=0.23)
        failed, log = self.compare([shifted, self.desktop], [self.core_only])
        self.assertFalse(failed, log)
        self.assertIn("1/1 desktop records", log)
        self.assertIn("Expected JS8Core-only matches: 1/1", log)

    def test_strict_without_core_only_expectations(self):
        failed, log = self.compare([self.desktop, self.core_only], [])
        self.assertTrue(failed, log)
        self.assertIn("EXTRA JS8CORE", log)

    def test_both_kinds_of_expected_record_are_required(self):
        for present, missing_source in ((self.desktop, "JS8Core-only"),
                                        (self.core_only, "desktop")):
            with self.subTest(missing=missing_source):
                failed, log = self.compare([present], [self.core_only])
                self.assertTrue(failed, log)
                self.assertIn(f"MISSING JS8CORE ({missing_source})", log)

    def test_duplicate_or_unexpected_extra_fails(self):
        unexpected = replace(self.core_only, payload_hex="4546")
        for extra in (self.core_only, unexpected):
            with self.subTest(extra=extra):
                failed, log = self.compare([self.desktop, self.core_only, extra], [self.core_only])
                self.assertTrue(failed, log)
                self.assertIn("EXTRA JS8CORE", log)

    def test_core_only_identity_is_exact(self):
        for changes in ({"file": "other.wav"}, {"kind": 2}, {"mode": 1}, {"payload_hex": "4546"}):
            with self.subTest(changes=changes):
                changed = replace(self.core_only, **changes)
                failed, log = self.compare([self.desktop, changed], [self.core_only])
                self.assertTrue(failed, log)
                self.assertIn("MISSING JS8CORE (JS8Core-only)", log)
                self.assertIn("EXTRA JS8CORE", log)

    def test_core_only_metadata_must_be_within_tolerance(self):
        for changes in ({"frequency_hz": 810.0}, {"xdt_seconds": 0.40}, {"snr": -18}, {"quality": 0.3}):
            with self.subTest(changes=changes):
                changed = replace(self.core_only, **changes)
                failed, log = self.compare([self.desktop, changed], [self.core_only])
                self.assertTrue(failed, log)
                self.assertIn("METADATA DIFF (JS8Core-only)", log)

    def test_core_only_expectation_cannot_duplicate_a_desktop_record(self):
        failed, log = self.compare([self.desktop, self.desktop], [self.desktop])
        self.assertTrue(failed, log)
        self.assertIn("also in desktop output", log)


if __name__ == "__main__":
    unittest.main()
