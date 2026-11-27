#!/usr/bin/env python3
"""Independent arithmetic and C++ CLI contracts for the M8.0 study."""

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import csv
import tempfile
import unittest
from decimal import Decimal


def load_study_module(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("weight_residency_break_even", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot import weight-residency study module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WeightResidencyBreakEvenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = pathlib.Path(__file__).resolve().parents[1]
        cls.study = load_study_module(cls.root / "experiments/weight_residency_break_even/run_study.py")
        binary = getattr(cls, "binary", None)
        if binary is None:
            cls.binary = (cls.root / "build/axissim").resolve()

    def test_hand_derived_break_even_arithmetic_and_sensitivity(self):
        # The two persistent tensors are 800 B + 12,800 B; frame 0 still loads them.
        saved_bytes = 13_600 * (30 - 1)
        self.assertEqual(saved_bytes, 394_400)
        dram_saved = Decimal(saved_bytes) * Decimal("15.0")
        dma_saved = Decimal(saved_bytes) * Decimal("1.5")
        self.assertEqual(dram_saved, Decimal("5916000.0"))
        self.assertEqual(dma_saved, Decimal("591600.0"))
        self.assertEqual(dram_saved + dma_saved, Decimal("6507600.0"))
        self.assertEqual((dram_saved + dma_saved) / 30, Decimal("216920.0"))
        self.assertEqual(Decimal(saved_bytes) * (Decimal("7.5") + Decimal("1.5")), Decimal("3549600.0"))
        self.assertEqual(Decimal(saved_bytes) * (Decimal("30") + Decimal("1.5")), Decimal("12423600.0"))
        self.assertEqual(Decimal(saved_bytes) * (Decimal("15") + Decimal("0.75")), Decimal("6211800.00"))
        self.assertEqual(Decimal(saved_bytes) * (Decimal("15") + Decimal("3")), Decimal("7099200"))

    def test_preregistered_one_at_a_time_cases_do_not_change_workload_controls(self):
        manifest, _ = self.study.load_manifest(self.root / "experiments/weight_residency_break_even/manifest.json")
        cases = self.study._case_specs(manifest)
        self.assertEqual([case["case_id"] for case in cases],
                         ["base", "dram_half", "dram_double", "dma_half", "dma_double"])
        self.assertEqual([(case["dram"], case["dma"]) for case in cases], [
            (Decimal("15.0"), Decimal("1.5")),
            (Decimal("7.50"), Decimal("1.5")),
            (Decimal("30.0"), Decimal("1.5")),
            (Decimal("15.0"), Decimal("0.75")),
            (Decimal("15.0"), Decimal("3.0")),
        ])
        self.assertEqual(manifest["workload"]["frames"], 30)
        self.assertEqual(manifest["architecture_controls"]["weight_sram_capacity_bytes"], 16384)

    def test_profile_and_mapping_inputs_independently_yield_expected_payload(self):
        profile = json.loads((self.root / "reference_data/workloads/mnist12_profile.json").read_text())
        baseline = json.loads((self.root / "configs/mappings/mnist12_baseline.json").read_text())
        resident = json.loads((self.root / "configs/mappings/mnist12_persistent_weights.json").read_text())
        operations = {entry["id"]: entry for entry in profile["operators"]}
        baseline_bytes = sum(op["activation_read_bytes"] + op["activation_write_bytes"] + op["weight_bytes"]
                             for op in profile["operators"])
        persistent_ids = {entry["operator_id"] for entry in resident["entries"]
                          if entry.get("weight_residency") == "persistent"}
        self.assertEqual(persistent_ids, {"Convolution28", "Convolution110"})
        self.assertTrue(all("weight_residency" not in entry or entry["weight_residency"] == "streamed"
                            for entry in baseline["entries"]))
        weights = sum(operations[operator_id]["weight_bytes"] for operator_id in persistent_ids)
        self.assertEqual(weights, 800 + 12_800)
        self.assertEqual(baseline_bytes, 290_176)
        self.assertEqual(weights * (30 - 1), 394_400)
        self.assertEqual(baseline_bytes * 30, 8_705_280)
        self.assertEqual(baseline_bytes * 30 - weights * 29, 8_310_880)

    def test_cpp_coefficient_sensitivities_match_independent_energy_oracle(self):
        if not self.binary.is_file():
            self.skipTest(f"C++ CLI not found: {self.binary}")
        manifest, _ = self.study.load_manifest(self.root / "experiments/weight_residency_break_even/manifest.json")
        config = self.root / manifest["workload"]["scenario_config"]
        controls = manifest["architecture_controls"]
        results = {}
        with tempfile.TemporaryDirectory(prefix="axissim-m8-sensitivity-") as temp_name:
            output = pathlib.Path(temp_name)
            for case in self.study._case_specs(manifest):
                results[case["case_id"]] = {}
                for arm, mapping_key in (("streamed", "no_residency_mapping"), ("resident", "persistent_mapping")):
                    result_path = output / f"{case['case_id']}_{arm}.json"
                    trace_path = output / f"{case['case_id']}_{arm}.csv"
                    mapping = self.study._mapping_override(self.root, controls[mapping_key])
                    command = [str(self.binary), "run", str(config)]
                    overrides = {
                        "dram_byte_pj": str(case["dram"]),
                        "dma_byte_pj": str(case["dma"]),
                        **manifest["static_power_overrides_w"],
                        "mapping_plan": mapping,
                        "weight_sram_capacity_bytes": str(controls["weight_sram_capacity_bytes"]),
                    }
                    for key, value in overrides.items():
                        command.extend(["--set", f"{key}={value}"])
                    command.extend(["--json", str(result_path), "--trace", str(trace_path)])
                    process = subprocess.run(command, cwd=self.root, capture_output=True, text=True, timeout=45)
                    self.assertEqual(process.returncode, 0, process.stderr)
                    result = json.loads(result_path.read_text(), parse_float=Decimal)
                    provenance = json.loads(pathlib.Path(str(result_path) + ".provenance.json").read_text())["provenance"]
                    results[case["case_id"]][arm] = (result, provenance)

                streamed, stream_prov = results[case["case_id"]]["streamed"]
                resident, resident_prov = results[case["case_id"]]["resident"]
                stream_config = dict(stream_prov["configuration"])
                resident_config = dict(resident_prov["configuration"])
                stream_config.pop("mapping_plan")
                resident_config.pop("mapping_plan")
                self.assertEqual(stream_config, resident_config)
                self.assertEqual(streamed["traffic"]["dram_bytes"] - resident["traffic"]["dram_bytes"], 394_400)
                self.assertEqual(streamed["energy"]["dram_pj"] - resident["energy"]["dram_pj"],
                                 Decimal(394_400) * case["dram"])
                self.assertEqual(streamed["energy"]["dma_pj"] - resident["energy"]["dma_pj"],
                                 Decimal(394_400) * case["dma"])
                for component in ("vector_pj", "npu_arithmetic_pj", "static_per_frame_uj"):
                    self.assertEqual(streamed["energy"][component], resident["energy"][component])

                if case["case_id"] == "base":
                    continue
                base_stream, _ = results["base"]["streamed"]
                base_resident, _ = results["base"]["resident"]
                for arm_result, base_result in ((streamed, base_stream), (resident, base_resident)):
                    self.assertEqual(arm_result["timing"], base_result["timing"])
                    self.assertEqual(arm_result["latency"], base_result["latency"])
                    self.assertEqual(arm_result["deadlines"], base_result["deadlines"])

    def test_actual_cpp_cli_pair_and_capacity_rejection(self):
        if not self.binary.is_file():
            self.skipTest(f"C++ CLI not found: {self.binary}")
        config = self.root / "configs/workloads/mnist12_profiled.conf"
        with tempfile.TemporaryDirectory(prefix="axissim-m8-pair-") as temp_name:
            output = pathlib.Path(temp_name)
            runs = {}
            commands = {}
            for arm, mapping in (
                ("streamed", "../mappings/mnist12_baseline.json"),
                ("resident", "../mappings/mnist12_persistent_weights.json"),
            ):
                result_path = output / f"{arm}.json"
                trace_path = output / f"{arm}.csv"
                command = [str(self.binary), "run", str(config)]
                for key in ("dram_byte_pj", "dma_byte_pj", "static_power_w", "npu_static_power_per_mac_w",
                            "rvv_static_power_per_lane_w", "sram_static_power_per_kib_w", "dram_interface_static_power_w"):
                    value = "15.0" if key == "dram_byte_pj" else "1.5" if key == "dma_byte_pj" else "0"
                    command.extend(["--set", f"{key}={value}"])
                command.extend(["--set", f"mapping_plan={mapping}", "--set", "weight_sram_capacity_bytes=16384",
                                "--json", str(result_path), "--trace", str(trace_path)])
                commands[arm] = command
                process = subprocess.run(command, cwd=self.root, capture_output=True, text=True, timeout=45)
                self.assertEqual(process.returncode, 0, process.stderr)
                provenance_path = pathlib.Path(str(result_path) + ".provenance.json")
                result_hash = hashlib.sha256(result_path.read_bytes()).hexdigest()
                provenance = json.loads(provenance_path.read_text())
                self.assertEqual(provenance["provenance"]["result"]["sha256"], result_hash)
                self.assertEqual(provenance["provenance"]["input_verification"]["status"], "exact_bytes_captured_and_used")
                self.assertRegex(provenance["provenance"]["source"]["revision"], r"^[0-9a-f]{40}$")
                self.assertEqual(provenance["provenance"]["executable"]["sha256"],
                                 hashlib.sha256(self.binary.read_bytes()).hexdigest())
                self.assertEqual(provenance["provenance"]["configuration"]["weight_sram_capacity_bytes"], 16384)
                self.assertEqual(provenance["provenance"]["configuration"]["dram_byte_pj"], 15)
                self.assertEqual(provenance["provenance"]["configuration"]["dma_byte_pj"], 1.5)
                self.assertEqual(provenance["provenance"]["configuration"]["static_power_w"], 0)
                self.assertTrue(trace_path.is_file())
                runs[arm] = json.loads(result_path.read_text(), parse_float=Decimal)
                runs[arm + "_artifacts"] = (result_path.read_bytes(), provenance_path.read_bytes(), trace_path.read_bytes())
            streamed, resident = runs["streamed"], runs["resident"]
            self.assertEqual(streamed["traffic"]["dram_bytes"], 8_705_280)
            self.assertEqual(resident["traffic"]["dram_bytes"], 8_310_880)
            self.assertEqual(streamed["profile_transfer_ledger"]["first_frame_bytes"], 290_176)
            self.assertEqual(resident["profile_transfer_ledger"]["first_frame_bytes"], 290_176)
            self.assertEqual(streamed["profile_transfer_ledger"]["steady_state_bytes"], 290_176)
            self.assertEqual(resident["profile_transfer_ledger"]["steady_state_bytes"], 276_576)
            self.assertEqual(resident["mapping_effects"]["persistent_weight_bytes_avoided_per_steady_frame"], 13_600)
            self.assertEqual(streamed["energy"]["dram_pj"] - resident["energy"]["dram_pj"], Decimal("5916000"))
            self.assertEqual(streamed["energy"]["dma_pj"] - resident["energy"]["dma_pj"], Decimal("591600"))
            self.assertEqual((streamed["energy"]["dram_pj"] - resident["energy"]["dram_pj"]) +
                             (streamed["energy"]["dma_pj"] - resident["energy"]["dma_pj"]),
                             Decimal(13_600 * 29) * (Decimal("15.0") + Decimal("1.5")))
            self.assertEqual(streamed["energy"]["vector_pj"], resident["energy"]["vector_pj"])
            self.assertEqual(streamed["energy"]["npu_arithmetic_pj"], resident["energy"]["npu_arithmetic_pj"])
            self.assertEqual(streamed["energy"]["static_per_frame_uj"], resident["energy"]["static_per_frame_uj"])
            self.assertEqual(streamed["energy"]["static_per_frame_uj"], Decimal(0))
            self.assertEqual(streamed["deadlines"]["vision_misses"], 0)
            self.assertEqual(resident["deadlines"]["vision_misses"], 0)
            self.assertEqual(streamed["deadlines"]["vision_jobs"], 30)
            self.assertEqual(resident["deadlines"]["vision_jobs"], 30)

            for arm in ("streamed", "resident"):
                result_path = output / f"{arm}_repeat.json"
                trace_path = output / f"{arm}_repeat.csv"
                repeat_command = copy.deepcopy(commands[arm])
                repeat_command[repeat_command.index("--json") + 1] = str(result_path)
                repeat_command[repeat_command.index("--trace") + 1] = str(trace_path)
                repeated = subprocess.run(repeat_command, cwd=self.root, capture_output=True, text=True, timeout=45)
                self.assertEqual(repeated.returncode, 0, repeated.stderr)
                provenance_path = pathlib.Path(str(result_path) + ".provenance.json")
                original_artifacts = runs[arm + "_artifacts"]
                self.assertEqual(result_path.read_bytes(), original_artifacts[0])
                self.assertEqual(provenance_path.read_bytes(), original_artifacts[1])
                self.assertEqual(trace_path.read_bytes(), original_artifacts[2])

            rejected = subprocess.run(
                [str(self.binary), "run", str(config), "--set", "mapping_plan=../mappings/mnist12_persistent_weights.json",
                 "--set", "weight_sram_capacity_bytes=8192"],
                cwd=self.root, capture_output=True, text=True, timeout=45)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("require 13600 bytes of weight SRAM", rejected.stderr)
            self.assertIn("configured capacity is 8192", rejected.stderr)

    def test_clean_checkout_runner_publishes_complete_verified_bundle(self):
        if not self.binary.is_file():
            self.skipTest(f"C++ CLI not found: {self.binary}")
        status = subprocess.run(
            ["git", "-C", str(self.root), "status", "--porcelain", "--untracked-files=all"],
            capture_output=True, text=True, check=True)
        if status.stdout.strip():
            self.skipTest("full-runner publication check requires a clean checkout; run it in a clean worktree")

        runner = self.root / "experiments/weight_residency_break_even/run_study.py"
        with tempfile.TemporaryDirectory(prefix="axissim-m8-runner-e2e-") as temp_name:
            output = pathlib.Path(temp_name) / "study"
            process = subprocess.run(
                ["python3", str(runner), "--binary", str(self.binary), "--output-dir", str(output)],
                cwd=self.root, capture_output=True, text=True, timeout=180)
            self.assertEqual(process.returncode, 0, process.stderr)
            study = json.loads((output / "study.json").read_text(), parse_float=Decimal)
            self.assertEqual(study["study_status"], "complete")
            self.assertEqual(len(study["cases"]), 5)
            self.assertTrue(all(case["status"] == "complete" for case in study["cases"]))
            self.assertEqual(study["capacity_negative_control"]["status"], "rejected_as_expected")
            self.assertEqual({item["status"] for item in study["reproducibility"].values()}, {"identical"})
            self.assertTrue(study["provenance"]["complete"])
            self.assertEqual(study["provenance"]["source_revisions"],
                             [subprocess.check_output(["git", "-C", str(self.root), "rev-parse", "HEAD"], text=True).strip()])
            self.assertEqual(study["provenance"]["executable_sha256s"],
                             [hashlib.sha256(self.binary.read_bytes()).hexdigest()])
            outcomes = {item["id"]: item["outcome"] for item in study["hypotheses"]}
            self.assertEqual(set(outcomes), {"H1", "H2", "H3", "H4", "H5"})
            self.assertNotIn("not_evaluable", outcomes.values())
            self.assertEqual(study["hypotheses"], self.study.hypothesis_outcomes(study))

            with (output / "summary.csv").open(newline="", encoding="utf-8") as source:
                rows = list(csv.DictReader(source))
            self.assertEqual(len(rows), 5)
            base = next(row for row in rows if row["case_id"] == "base")
            self.assertEqual(Decimal(base["max_additional_unpriced_sram_energy_pj_over_observation"]),
                             Decimal("6507600"))
            self.assertEqual(Decimal(base["max_additional_unpriced_sram_energy_pj_per_completed_frame"]),
                             Decimal("216920"))
            report = (output / "report.md").read_text(encoding="utf-8")
            self.assertIn("6.5076", report)
            self.assertIn("0.21692", report)

            inventory = (output / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines()
            self.assertTrue(inventory)
            listed = set()
            for entry in inventory:
                digest, relative = entry.split("  ", 1)
                artifact = (output / relative).resolve()
                artifact.relative_to(output.resolve())
                self.assertRegex(digest, r"^[0-9a-f]{64}$")
                self.assertTrue(artifact.is_file(), relative)
                self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(), digest, relative)
                listed.add(relative)
            self.assertNotIn("SHA256SUMS.txt", listed)
            self.assertIn("study.json", listed)
            self.assertIn("summary.csv", listed)
            self.assertIn("report.md", listed)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    args = parser.parse_args()
    WeightResidencyBreakEvenTests.root = args.source_root.resolve()
    WeightResidencyBreakEvenTests.binary = args.binary.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(WeightResidencyBreakEvenTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
