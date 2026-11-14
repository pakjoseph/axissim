#!/usr/bin/env python3
"""Small deterministic tests for M5 replay artifact path validation."""

from __future__ import annotations

import runpy
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERIFIER = runpy.run_path(str(ROOT / "tools" / "verify_m5_study_replay.py"))
contained_study_path = VERIFIER["contained_study_path"]
VerificationError = VERIFIER["VerificationError"]


class StudyArtifactPathTests(unittest.TestCase):
    def test_normal_relative_path_is_resolved_inside_study(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "traces").mkdir()
            self.assertEqual(contained_study_path(root, "traces/frame.jsonl"), root.resolve() / "traces/frame.jsonl")

    def test_parent_and_absolute_paths_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "study"
            root.mkdir()
            for relative in ("../outside.json", str(root.parent / "outside.json")):
                with self.subTest(relative=relative):
                    with self.assertRaises(VerificationError):
                        contained_study_path(root, relative)


if __name__ == "__main__":
    unittest.main()
