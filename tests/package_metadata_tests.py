#!/usr/bin/env python3
"""Checks configured package identity, license staging, and tag mismatch rejection."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=pathlib.Path)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    build = args.build_dir.resolve()
    metadata_path = build / "package-assets/package-provenance.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    revision = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if metadata.get("schema_version") != "axissim.package-provenance.v1":
        raise AssertionError("unsupported package provenance schema")
    if metadata.get("source_revision") != revision:
        raise AssertionError("package metadata does not bind the configured source revision")
    tree_status = subprocess.run(
        ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=all"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    expected_tree_state = "clean" if not tree_status else "modified"
    if metadata.get("source_tree_state") != expected_tree_state:
        raise AssertionError("package metadata does not report tracked and untracked source-tree state")
    if metadata.get("artifact_kind") != "validation" or metadata.get("release_tag") != "unknown":
        raise AssertionError("untagged package must be labeled as a validation artifact")
    if not metadata.get("package_id", "").startswith("validation-"):
        raise AssertionError("validation package ID is missing")
    if metadata["build"].get("compiler_version") in (None, "", "unknown"):
        raise AssertionError("configured compiler identity is missing")
    if metadata["build"].get("cxx_standard") != "20":
        raise AssertionError("package metadata must record C++20")
    if (build / "package-assets/LICENSE").read_bytes() != (root / "LICENSE").read_bytes():
        raise AssertionError("repository LICENSE was not staged exactly")
    if (build / "package-assets/licenses/LGPL-3.0.txt").stat().st_size < 7000:
        raise AssertionError("Qt LGPL license text is missing or truncated")
    notices = (build / "package-assets/THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
    normalized_notices = " ".join(notices.split())
    if "source-code" not in normalized_notices or "validation artifacts only" not in normalized_notices:
        raise AssertionError("package notices must preserve Qt redistribution limitations")
    if "not yet prove a complete per-runtime-file license mapping" not in normalized_notices:
        raise AssertionError("package notices must not overstate the installation SBOM as a runtime license map")
    if ("vc_redist.x64.exe" not in normalized_notices or
            "Microsoft Visual Studio license terms" not in normalized_notices):
        raise AssertionError("package notices must disclose the bundled Microsoft runtime installer and license boundary")
    import sys
    sys.path.insert(0, str(root / "tools"))
    from verify_package_inputs import validate_all_tracked_inputs, validate_package_inputs
    from verify_extracted_package import (
        is_application_pe_for_arch_check,
        is_msvc_runtime_dll,
        package_inventory_root,
        package_member_path,
        unresolved_windows_imports,
        verify_windows_vc_redist,
    )
    if not is_msvc_runtime_dll("VCRUNTIME140.dll") or not is_msvc_runtime_dll("msvcp140_2.dll"):
        raise AssertionError("MSVC compiler runtime DLLs must be classified as packaged dependencies")
    if is_msvc_runtime_dll("kernel32.dll") or is_msvc_runtime_dll("ucrtbase.dll"):
        raise AssertionError("Windows OS runtime DLLs must not be classified as MSVC redistributables")
    system_dlls = {"kernel32.dll", "vcruntime140.dll"}
    missing_runtime = unresolved_windows_imports(
        "AxisSim.exe", ["KERNEL32.dll", "VCRUNTIME140.dll"], set(), system_dlls
    )
    if missing_runtime != [
        "AxisSim.exe -> VCRUNTIME140.dll (packaged VC redistributable prerequisite is missing or not installed)"
    ]:
        raise AssertionError("a host-installed MSVC runtime must not satisfy an undeclared package prerequisite")
    if unresolved_windows_imports(
        "AxisSim.exe", ["KERNEL32.dll", "VCRUNTIME140.dll"],
        {"vcruntime140.dll"}, system_dlls,
    ):
        raise AssertionError("a packaged MSVC runtime and Windows system import should resolve")
    if unresolved_windows_imports(
        "AxisSim.exe", ["KERNEL32.dll", "VCRUNTIME140.dll"], set(), system_dlls,
        packaged_vc_redist_installer=True,
    ):
        raise AssertionError("a declared bundled VC redistributable and installed system runtime should resolve")
    if not unresolved_windows_imports(
        "AxisSim.exe", ["VCRUNTIME140.dll"], set(), {"kernel32.dll"},
        packaged_vc_redist_installer=True,
    ):
        raise AssertionError("a declared redistributable must still be installed to satisfy the package imports")

    with tempfile.TemporaryDirectory(prefix="axissim-qt-inventory-") as temporary:
        fixture = pathlib.Path(temporary)
        sbom_root = fixture / "third-party/qt-installation-sboms/share/qt/sbom"
        sbom_root.mkdir(parents=True)
        (sbom_root / "qtbase.spdx").write_text("SPDXVersion: SPDX-2.3\n", encoding="utf-8")
        (sbom_root / "qtgui.spdx.json").write_text('{"spdxVersion":"SPDX-2.3"}\n', encoding="utf-8")
        (fixture / "Qt6Core.dll").write_bytes(b"synthetic runtime file")
        redist_installer = fixture / "vc_redist.x64.exe"
        redist_installer.write_bytes(b"synthetic VC redistributable installer")
        output_dir = fixture / "inventory-output"
        subprocess.run(
            [
                sys.executable, str(root / "tools/generate_package_runtime_inventory.py"),
                "--root", str(fixture), "--platform", "windows", "--qt-version", "6.8.3",
                "--runtime-mode", "bundled-dynamic", "--output-dir", str(output_dir),
            ],
            check=True, capture_output=True, text=True,
        )
        inventory = json.loads((output_dir / "qt-runtime-inventory.json").read_text(encoding="utf-8"))
        if inventory.get("schema_version") != "axissim.package-runtime-inventory.v2":
            raise AssertionError("runtime inventory must version its VC redistributable prerequisite contract")
        if inventory.get("qt_installation_sbom_status") != "included_installation_catalog_not_runtime_mapping":
            raise AssertionError("runtime inventory must label the SBOMs as a catalog, not a file mapping")
        sboms = inventory.get("qt_installation_sboms", [])
        expected_sbom_paths = {
            "third-party/qt-installation-sboms/share/qt/sbom/qtbase.spdx",
            "third-party/qt-installation-sboms/share/qt/sbom/qtgui.spdx.json",
        }
        if {item.get("path") for item in sboms} != expected_sbom_paths:
            raise AssertionError("runtime inventory must retain every SBOM path without basename collisions")
        for item in sboms:
            if not re.fullmatch(r"[0-9a-f]{64}", item.get("sha256", "")):
                raise AssertionError("runtime inventory must hash every installation SBOM")
            if not (fixture / item["path"]).is_file():
                raise AssertionError("runtime inventory SBOM path must exist in the package root")
        if len(inventory.get("runtime_files", [])) != 1:
            raise AssertionError("runtime inventory must retain the exact native package file list")
        redist_identity = inventory.get("windows_vc_redist")
        if (not isinstance(redist_identity, dict) or
                redist_identity.get("path") != "vc_redist.x64.exe" or
                redist_identity.get("sha256") != hashlib.sha256(redist_installer.read_bytes()).hexdigest() or
                redist_identity.get("redistributable_family") != "microsoft-vc-runtime-14-x64"):
            raise AssertionError("Windows inventory must bind the packaged VC redist prerequisite by SHA-256")
        if verify_windows_vc_redist(fixture, inventory) != redist_installer.resolve():
            raise AssertionError("Windows verifier must resolve the inventoried VC redist within package root")
        if (is_application_pe_for_arch_check(redist_installer, redist_installer) or
                not is_application_pe_for_arch_check(fixture / "Qt6Core.dll", redist_installer)):
            raise AssertionError("only the exact inventoried VC bootstrapper may bypass app PE architecture checks")
        redist_installer.write_bytes(b"modified VC redistributable installer")
        try:
            verify_windows_vc_redist(fixture, inventory)
        except AssertionError:
            pass
        else:
            raise AssertionError("Windows verifier accepted a changed VC redistributable hash")
        (fixture / "Qt6Core.dll").unlink()
        system_output = fixture / "system-inventory-output"
        subprocess.run(
            [
                sys.executable, str(root / "tools/generate_package_runtime_inventory.py"),
                "--root", str(fixture), "--platform", "linux", "--qt-version", "6.8.3",
                "--runtime-mode", "system-dynamic", "--output-dir", str(system_output),
            ],
            check=True, capture_output=True, text=True,
        )
        system_inventory = json.loads((system_output / "qt-runtime-inventory.json").read_text(encoding="utf-8"))
        if ({item.get("path") for item in system_inventory.get("qt_installation_sboms", [])} != expected_sbom_paths or
                system_inventory.get("qt_installation_sbom_status") != "included_installation_catalog_not_runtime_mapping" or
                system_inventory.get("runtime_files") != [] or
                system_inventory.get("windows_vc_redist") is not None):
            raise AssertionError("system-Qt package assets must retain available SBOM hashes without inventing bundled libraries")
        deb_root = fixture / "deb-root"
        deb_docs = deb_root / "usr/share/doc/axissim"
        deb_sbom = deb_docs / "third-party/qt-installation-sboms/share/qt/sbom/qtbase.spdx"
        deb_sbom.parent.mkdir(parents=True)
        deb_sbom.write_text("SPDXVersion: SPDX-2.3\n", encoding="utf-8")
        if package_inventory_root(deb_root, deb_docs, "deb") != deb_docs:
            raise AssertionError("Debian package inventory paths must resolve relative to installed documentation")
        if package_member_path(
            package_inventory_root(deb_root, deb_docs, "deb"),
            "third-party/qt-installation-sboms/share/qt/sbom/qtbase.spdx",
            "Debian SBOM fixture",
            extraction_root=deb_root,
        ) != deb_sbom.resolve():
            raise AssertionError("Debian package verifier resolved an SBOM against the wrong extraction root")
        if package_inventory_root(deb_root, deb_docs, "macos") != deb_root:
            raise AssertionError("macOS inventory paths must remain relative to app Contents")
        outside_root = fixture / "outside-package"
        outside_root.mkdir()
        (outside_root / "external.spdx").write_text("external file\n", encoding="utf-8")
        try:
            package_member_path(
                outside_root, "external.spdx", "Debian SBOM fixture", extraction_root=deb_root
            )
        except AssertionError:
            pass
        else:
            raise AssertionError("Debian inventory verifier accepted a base outside the extracted package")
        symlink_root = fixture / "symlink-package-root"
        symlink_docs = symlink_root / "usr/share/doc/axissim"
        symlink_docs.parent.mkdir(parents=True)
        try:
            symlink_docs.symlink_to(outside_root, target_is_directory=True)
        except OSError:
            # Some Windows environments do not grant symlink creation rights;
            # the direct outside-base check above still covers containment.
            pass
        else:
            try:
                package_member_path(
                    package_inventory_root(symlink_root, symlink_docs, "deb"),
                    "external.spdx",
                    "Debian SBOM fixture",
                    extraction_root=symlink_root,
                )
            except AssertionError:
                pass
            else:
                raise AssertionError("Debian inventory verifier followed a documentation symlink outside the package")
        for unsafe_path in ("../outside.spdx", "sub/../../outside.spdx", "/outside.spdx", "C:/outside.spdx"):
            try:
                package_member_path(fixture, unsafe_path, "synthetic inventory item")
            except AssertionError:
                pass
            else:
                raise AssertionError(f"package verifier accepted path outside its extraction root: {unsafe_path}")
    workflow = (root / ".github/workflows/release-packages.yml").read_text(encoding="utf-8")
    cmake_source = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if 'run: ./scripts/package_linux.sh build/package-linux-release "$AXISSIM_RELEASE_TAG"' not in workflow:
        raise AssertionError("Linux release tag must enter the shell through an environment variable")
    if 'ReleaseTag "%AXISSIM_RELEASE_TAG%"' not in workflow:
        raise AssertionError("Windows release tag must enter the developer-shell package process through an environment variable")
    if 'run: ./scripts/package_macos.sh build/package-macos-release "$AXISSIM_RELEASE_TAG"' not in workflow:
        raise AssertionError("macOS release tag must enter the shell through an environment variable")
    if "build/package-windows-release" not in workflow:
        raise AssertionError("Windows package build must stay under the ignored build directory")
    if "      - 'src/**'" not in workflow:
        raise AssertionError("native package verification must run when any packaged C++ source changes")
    for input_prefix in ("configs", "reference_data", "resources"):
        if f"      - '{input_prefix}/**'" not in workflow:
            raise AssertionError(f"native package validation must run when shipped {input_prefix} inputs change")
    if "VsDevCmd.bat" not in workflow or "VCINSTALLDIR" not in workflow:
        raise AssertionError("Windows package validation must run windeployqt in a C++ developer environment")
    if "InstallVcRuntimeForValidation" not in workflow:
        raise AssertionError("Windows package validation must exercise the bundled VC redistributable prerequisite")
    windows_script = (root / "scripts/package_windows.ps1").read_text(encoding="utf-8")
    if "$env:VCINSTALLDIR" not in windows_script:
        raise AssertionError("Windows package script must reject a missing Visual Studio runtime source")
    if "vc_redist.x64.exe" not in windows_script:
        raise AssertionError("Windows package script must require the bundled VC redistributable installer")
    if "[System.IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Extracted)" not in windows_script:
        raise AssertionError("Windows package verification must use the portable two-argument ZIP extraction API")
    if "Expand-Archive" in windows_script:
        raise AssertionError("Windows package verification must not depend on the runner-specific Expand-Archive overload")
    for script_name in ("package_linux.sh", "package_macos.sh", "package_windows.ps1"):
        script_text = (root / "scripts" / script_name).read_text(encoding="utf-8")
        if "AXISSIM_PACKAGE_BUILD=ON" not in script_text:
            raise AssertionError(f"{script_name} must enable strict package-input verification")
    linux_script = (root / "scripts/package_linux.sh").read_text(encoding="utf-8")
    macos_script = (root / "scripts/package_macos.sh").read_text(encoding="utf-8")
    if 'cmake -E remove_directory "$stage"' not in linux_script or "--clean-first" not in linux_script:
        raise AssertionError("Linux package builds must remove stale staging files and rebuild cleanly")
    if ('rm -f "$archive"' not in linux_script or "-name 'axissim*.deb' -delete" not in linux_script or
            "Expected exactly one newly generated axissim Debian package" not in linux_script):
        raise AssertionError("Linux packaging must discard stale final outputs and require one fresh Debian package")
    if 'rm -rf "$stage"' not in linux_script:
        raise AssertionError("Linux package failure cleanup must remove an unverified stage")
    if linux_script.index('cmake -E remove_directory "$stage"') > linux_script.index("cmake -S "):
        raise AssertionError("Linux package staging must be cleared before configuration can fail")
    if 'if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }' not in windows_script or "--clean-first" not in windows_script:
        raise AssertionError("Windows package builds must remove stale staging files and rebuild cleanly")
    if ('if (Test-Path $Archive) { Remove-Item $Archive -Force }' not in windows_script or
            "if (Test-Path $Archive) { Remove-Item $Archive -Force -ErrorAction SilentlyContinue }" not in windows_script):
        raise AssertionError("Windows packaging must clear stale archives before validation and on failure")
    if "existing runtime; package smoke tests will use that host runtime" not in windows_script:
        raise AssertionError("Windows packaging must distinguish installer exit 1638 from bundled runtime installation")
    if "succeeded; a restart is required. Smoke tests run in the current host environment." not in windows_script:
        raise AssertionError("Windows packaging must report restart-required installation without a DLL-version claim")
    if windows_script.index('if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }') > windows_script.index("cmake -S "):
        raise AssertionError("Windows package staging must be cleared before configuration can fail")
    if 'cmake -E remove_directory "$staging"' not in macos_script:
        raise AssertionError("macOS package build must remove successful DMG staging contents")
    if 'rm -f "$dmg"' not in macos_script or "if [[ $status -ne 0 ]]; then" not in macos_script:
        raise AssertionError("macOS packaging must clear stale DMGs and remove failed validation output")
    if (macos_script.count('rm -rf "$app" "$staging"') != 2 or
            macos_script.index('rm -rf "$app" "$staging"') > macos_script.index("cmake -S ")):
        raise AssertionError("macOS app and DMG staging must be cleared before configuration and on failure")
    if "runs-on: macos-15" not in workflow:
        raise AssertionError("Qt 6.8.3 package validation must avoid the incompatible macOS 26 SDK")
    if "tools/verify_package_inputs.py" not in workflow:
        raise AssertionError("native package validation must run when the package-input verifier changes")
    if ("AXISSIM_PACKAGE_INPUT_GUARD_COMMAND" not in cmake_source or
            cmake_source.count("${AXISSIM_PACKAGE_INPUT_GUARD_COMMAND}") != 2 or
            cmake_source.count(
                "add_custom_command(TARGET axissim-gui POST_BUILD\n"
                "                         ${AXISSIM_PACKAGE_INPUT_GUARD_COMMAND}\n"
                "                         COMMAND ${CMAKE_COMMAND} -E"
            ) != 2):
        raise AssertionError("package-input integrity must be checked immediately before both native package staging paths")
    if "upload-artifact" in workflow or "create-release" in workflow:
        raise AssertionError("package validation workflow must not publish or upload package outputs")
    readme = (root / "README.md").read_text(encoding="utf-8")
    if "does not retain them as downloadable Actions artifacts" not in readme:
        raise AssertionError("README must distinguish in-run package files from retained Actions artifacts")
    package_docs = " ".join(
        (root / "docs/package-provenance-and-licensing.md").read_text(encoding="utf-8").split()
    )
    if ("does not verify imported symbol exports" not in package_docs or
            "smoke tests cover only the dependency paths they exercise" not in package_docs):
        raise AssertionError("Windows dependency verification limitations must be explicit")
    if "rejects modified, untracked, or ignored files beneath `configs/` and `reference_data/`" not in package_docs:
        raise AssertionError("package input provenance guard must be documented")
    if "-InstallVcRuntimeForValidation" not in package_docs or "elevated Developer PowerShell" not in package_docs:
        raise AssertionError("Windows local package instructions must explain the runtime-validation prerequisite")
    if "Exit 1638 means another runtime is already installed" not in package_docs:
        raise AssertionError("Windows package docs must distinguish installed-host-runtime fallback from bundled installation")
    if ("3010 indicates success with a reboot required" not in package_docs or
            "do not record the exact runtime DLL version loaded" not in package_docs):
        raise AssertionError("Windows package docs must distinguish installer status from runtime-version verification")

    with tempfile.TemporaryDirectory(prefix="axissim-tag-mismatch-") as temporary:
        temporary_root = pathlib.Path(temporary)
        clean_source = temporary_root / "source"
        clean_source.mkdir()
        tracked_paths = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=True,
            capture_output=True,
        ).stdout.split(b"\0")
        package_guard_path = b"tools/verify_package_inputs.py"
        if package_guard_path not in tracked_paths and (root / os.fsdecode(package_guard_path)).is_file():
            tracked_paths.append(package_guard_path)
        for raw_path in tracked_paths:
            if not raw_path:
                continue
            relative_path = pathlib.Path(os.fsdecode(raw_path))
            source_path = root / relative_path
            destination_path = clean_source / relative_path
            destination_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_path, destination_path)
        subprocess.run(["git", "-C", str(clean_source), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "config", "user.name", "Package test"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "config", "user.email", "package-test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "add", "-f", "."], check=True)
        subprocess.run(["git", "-C", str(clean_source), "commit", "-qm", "fixture source"], check=True)
        first_commit = subprocess.run(
            ["git", "-C", str(clean_source), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        subprocess.run(["git", "-C", str(clean_source), "tag", "existing-tag"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "branch", "branch-only-tag"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "checkout", "-q", first_commit], check=True)
        branch_only = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "branch-only-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_RELEASE_TAG=branch-only-tag",
            ],
            capture_output=True,
            text=True,
        )
        if branch_only.returncode == 0:
            raise AssertionError("a branch without a same-named tag was accepted as a release tag")
        matching = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "matching-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_RELEASE_TAG=existing-tag",
            ],
            capture_output=True,
            text=True,
        )
        if matching.returncode != 0:
            raise AssertionError(f"a tag resolving to the exact source revision was rejected: {matching.stdout}{matching.stderr}")
        release_metadata = json.loads(
            (temporary_root / "matching-build/package-assets/package-provenance.json").read_text(encoding="utf-8")
        )
        if (release_metadata.get("artifact_kind") != "release" or
                release_metadata.get("release_tag") != "existing-tag" or
                release_metadata.get("source_revision") != first_commit or
                release_metadata.get("package_id") != "existing-tag"):
            raise AssertionError("verified package release identity does not bind the matching tag and source revision")
        outside_configs = temporary_root / "outside-configs"
        outside_configs.mkdir()
        configs_path = clean_source / "configs"
        configs_backup = clean_source / "configs-symlink-target"
        configs_path.rename(configs_backup)
        try:
            try:
                configs_path.symlink_to(outside_configs, target_is_directory=True)
            except OSError:
                pass
            else:
                symlinked_package_errors = validate_package_inputs(clean_source, first_commit)
                if not any("package input directory is a symlink" in item for item in symlinked_package_errors):
                    raise AssertionError("package input verification accepted a symlinked configs/ root")
        finally:
            if configs_path.is_symlink():
                configs_path.unlink()
            configs_backup.rename(configs_path)

        source_path = clean_source / "src"
        source_backup = clean_source / "src-symlink-target"
        source_path.rename(source_backup)
        try:
            try:
                source_path.symlink_to(source_backup, target_is_directory=True)
            except OSError:
                pass
            else:
                symlinked_source_errors = validate_all_tracked_inputs(clean_source, first_commit)
                if not any("release source path has a symlinked parent: src" in item for item in symlinked_source_errors):
                    raise AssertionError("release source verification accepted a symlinked source parent")
        finally:
            if source_path.is_symlink():
                source_path.unlink()
            source_backup.rename(source_path)
        source_file = clean_source / "src/gui/main.cpp"
        original_source = source_file.read_bytes()
        source_file.write_bytes(original_source + b"\n// hidden release-source mutation\n")
        subprocess.run(
            ["git", "-C", str(clean_source), "update-index", "--assume-unchanged", "src/gui/main.cpp"],
            check=True,
        )
        hidden_source_release = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "hidden-source-release-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_RELEASE_TAG=existing-tag",
            ],
            capture_output=True,
            text=True,
        )
        hidden_source_output = re.sub(
            r"\s+", " ", hidden_source_release.stdout + hidden_source_release.stderr
        )
        subprocess.run(
            ["git", "-C", str(clean_source), "update-index", "--no-assume-unchanged", "src/gui/main.cpp"],
            check=True,
        )
        source_file.write_bytes(original_source)
        if (hidden_source_release.returncode == 0 or
                "Release source verification failed" not in hidden_source_output or
                "src/gui/main.cpp" not in hidden_source_output):
            raise AssertionError(
                f"release-tag configuration accepted an assume-unchanged source modification: {hidden_source_output}"
            )
        untracked_input = clean_source / "configs" / "untracked-package-input.conf"
        untracked_input.write_text("scenario_name = untracked_package_input\n", encoding="utf-8")
        untracked_release = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "untracked-release-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_RELEASE_TAG=existing-tag",
            ],
            capture_output=True,
            text=True,
        )
        if untracked_release.returncode == 0 or "clean source tree" not in (untracked_release.stdout + untracked_release.stderr):
            raise AssertionError("an untracked configuration that would ship in the package was accepted")
        untracked_input.unlink()
        ignored_input = clean_source / "configs" / "workloads" / "ignored-package-input.csv"
        ignored_input.parent.mkdir(parents=True, exist_ok=True)
        ignored_input.write_text("untracked ignored package input\n", encoding="utf-8")
        ignored_check = subprocess.run(
            ["git", "-C", str(clean_source), "check-ignore", "-q", "configs/workloads/ignored-package-input.csv"],
            capture_output=True,
            text=True,
        )
        if ignored_check.returncode != 0:
            raise AssertionError("ignored package-input fixture is not covered by the repository ignore rules")
        ignored_package_build = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "ignored-package-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_PACKAGE_BUILD=ON",
            ],
            capture_output=True,
            text=True,
        )
        ignored_output = ignored_package_build.stdout + ignored_package_build.stderr
        if (ignored_package_build.returncode == 0 or
                "Package builds reject modified, untracked, or ignored files" not in ignored_output or
                "ignored-package-input.csv" not in ignored_output):
            raise AssertionError(f"package build did not reject an ignored file under configs/: {ignored_output}")
        ignored_developer_build = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "ignored-developer-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
            ],
            capture_output=True,
            text=True,
        )
        if ignored_developer_build.returncode != 0:
            raise AssertionError(
                "ordinary developer configuration was blocked by the package-only input guard: "
                f"{ignored_developer_build.stdout}{ignored_developer_build.stderr}"
            )
        ignored_input.unlink()

        untracked_package_input = clean_source / "configs" / "untracked-package-input.conf"
        untracked_package_input.write_text("scenario_name = untracked_package_input\n", encoding="utf-8")
        untracked_package_build = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "untracked-package-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_PACKAGE_BUILD=ON",
            ],
            capture_output=True,
            text=True,
        )
        untracked_package_output = untracked_package_build.stdout + untracked_package_build.stderr
        if (untracked_package_build.returncode == 0 or
                "Package builds reject modified, untracked, or ignored files" not in untracked_package_output or
                "untracked-package-input.conf" not in untracked_package_output):
            raise AssertionError(f"package build did not reject an untracked configs/ file: {untracked_package_output}")
        untracked_package_input.unlink()

        tracked_manifest = clean_source / "configs/baseline/manifest.conf"
        original_manifest = tracked_manifest.read_bytes()
        tracked_manifest.write_bytes(original_manifest + b"\n# package-guard mutation\n")
        modified_package_build = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "modified-package-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_PACKAGE_BUILD=ON",
            ],
            capture_output=True,
            text=True,
        )
        modified_package_output = modified_package_build.stdout + modified_package_build.stderr
        if (modified_package_build.returncode == 0 or
                "Package builds reject modified, untracked, or ignored files" not in modified_package_output or
                "configs/baseline/manifest.conf" not in modified_package_output):
            raise AssertionError(f"package build did not reject a modified tracked configuration: {modified_package_output}")
        tracked_manifest.write_bytes(original_manifest)

        for index_flag, flag_args in (
            ("assume-unchanged", ("--assume-unchanged", "configs/baseline/manifest.conf")),
            ("skip-worktree", ("--skip-worktree", "configs/baseline/manifest.conf")),
        ):
            tracked_manifest.write_bytes(original_manifest + f"\n# {index_flag} mutation\n".encode("ascii"))
            subprocess.run(["git", "-C", str(clean_source), "update-index", *flag_args], check=True)
            hidden_change_build = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(clean_source),
                    "-B",
                    str(temporary_root / f"{index_flag}-package-build"),
                    "-DAXISSIM_BUILD_GUI=OFF",
                    "-DAXISSIM_PACKAGE_BUILD=ON",
                ],
                capture_output=True,
                text=True,
            )
            hidden_change_output = re.sub(
                r"\s+", " ", hidden_change_build.stdout + hidden_change_build.stderr
            )
            if (hidden_change_build.returncode == 0 or
                    "Package input verification failed" not in hidden_change_output or
                    "configs/baseline/manifest.conf" not in hidden_change_output):
                raise AssertionError(
                    f"package build accepted a {index_flag} change to a tracked input: {hidden_change_output}"
                )
            subprocess.run(
                ["git", "-C", str(clean_source), "update-index", f"--no-{index_flag}", "configs/baseline/manifest.conf"],
                check=True,
            )
            tracked_manifest.write_bytes(original_manifest)

        ignored_reference = clean_source / "reference_data" / "ignored-package-input.csv"
        ignored_reference.parent.mkdir(parents=True, exist_ok=True)
        ignored_reference.write_text("ignored reference payload\n", encoding="utf-8")
        reference_ignore_check = subprocess.run(
            ["git", "-C", str(clean_source), "check-ignore", "-q", "reference_data/ignored-package-input.csv"],
            capture_output=True,
            text=True,
        )
        if reference_ignore_check.returncode != 0:
            raise AssertionError("ignored reference-data fixture is not covered by repository ignore rules")
        ignored_reference_build = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "ignored-reference-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_PACKAGE_BUILD=ON",
            ],
            capture_output=True,
            text=True,
        )
        ignored_reference_output = ignored_reference_build.stdout + ignored_reference_build.stderr
        if (ignored_reference_build.returncode == 0 or
                "Package builds reject modified, untracked, or ignored files" not in ignored_reference_output or
                "ignored-package-input.csv" not in ignored_reference_output):
            raise AssertionError(f"package build did not reject ignored reference data: {ignored_reference_output}")
        ignored_reference.unlink()

        late_input_build = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "late-input-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_PACKAGE_BUILD=ON",
            ],
            capture_output=True,
            text=True,
        )
        if late_input_build.returncode != 0:
            raise AssertionError(
                f"clean package inputs were rejected before the post-configure change test: "
                f"{late_input_build.stdout}{late_input_build.stderr}"
            )
        late_ignored_input = clean_source / "reference_data" / "late-ignored-input.csv"
        late_ignored_input.write_text("late ignored input\n", encoding="utf-8")
        late_guard_result = subprocess.run(
            [
                sys.executable,
                str(root / "tools/verify_package_inputs.py"),
                "--source-root",
                str(clean_source),
                "--revision",
                first_commit,
            ],
            capture_output=True,
            text=True,
        )
        if (late_guard_result.returncode == 0 or
                "late-ignored-input.csv" not in (late_guard_result.stdout + late_guard_result.stderr)):
            raise AssertionError("the package-input guard failed to detect data added after CMake configuration")
        late_input_build_result = subprocess.run(
            ["cmake", "--build", str(temporary_root / "late-input-build"), "--target", "axissim", "--parallel", "2"],
            capture_output=True,
            text=True,
        )
        late_input_output = re.sub(
            r"\s+", " ", late_input_build_result.stdout + late_input_build_result.stderr
        )
        if (late_input_build_result.returncode == 0 or
                "Package builds reject modified, untracked, or ignored files" not in late_input_output or
                "late-ignored-input.csv" not in late_input_output):
            raise AssertionError(
                f"a post-configure ignored input was not rejected before staging: {late_input_output}"
            )
        late_ignored_input.unlink()

        no_git_source = temporary_root / "source-without-git"
        shutil.copytree(clean_source, no_git_source, ignore=shutil.ignore_patterns(".git"))
        no_git_package_build = subprocess.run(
            [
                "cmake",
                "-S",
                str(no_git_source),
                "-B",
                str(temporary_root / "no-git-package-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_PACKAGE_BUILD=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE",
            ],
            capture_output=True,
            text=True,
        )
        no_git_output = no_git_package_build.stdout + no_git_package_build.stderr
        normalized_no_git_output = re.sub(r"\s+", " ", no_git_output)
        if (no_git_package_build.returncode == 0 or
                "Package builds require a verifiable Git source revision and package-input status" not in normalized_no_git_output):
            raise AssertionError(f"package build did not fail closed without Git source identity: {no_git_output}")

        (clean_source / "tag-fixture.txt").write_text("second revision\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(clean_source), "add", "tag-fixture.txt"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "commit", "-qm", "second fixture"], check=True)
        if subprocess.run(["git", "-C", str(clean_source), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip() == first_commit:
            raise AssertionError("tag mismatch fixture did not create distinct revisions")
        mismatch = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_RELEASE_TAG=existing-tag",
            ],
            capture_output=True,
            text=True,
        )
        combined = mismatch.stdout + mismatch.stderr
        normalized = re.sub(r"\s+", " ", combined)
        if mismatch.returncode == 0 or "does not resolve to packaged source revision" not in normalized:
            raise AssertionError(f"a mismatched release tag was not rejected explicitly: {combined}")

    print("package metadata contract passed: source revision, tree state, validation identity, exact tag refs, licenses, fail-closed Git identity, and modified/untracked/ignored package-input rejection")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
