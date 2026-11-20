# Package provenance and licensing

The native package scripts build and test validation artifacts on their target
operating systems. They do not publish or tag a release. The extracted package
contains the repository `LICENSE`, this project's third-party notice, Qt's
LGPLv3 license text, machine-readable package provenance, and a package-local
inventory of native runtime files and their SHA-256 values.

## Qt and other runtime dependencies

The AxisSim GUI links Qt 6 Core, Gui, and Widgets. Windows and macOS use
dynamically bundled Qt runtimes selected by `windeployqt` and `macdeployqt`.
The Windows packaging command requires a Visual Studio developer environment
with `VCINSTALLDIR` set. Its extracted-package check requires imported MSVC
redistributable DLLs to be present in the package rather than silently
accepting a runner-installed copy from `System32` or `SysWOW64`.
Linux packages intentionally use system Qt libraries and platform plugins;
the portable tarball documents that dependency, and the Debian package uses
the shared-library dependency scan plus explicit dependencies for every
Qt runtime package resolved from the extracted binaries and the Qt QPA plugin package. The Linux
tarball is not a self-contained application bundle.

`third-party/qt-runtime-inventory.json` identifies and hashes the shared native
libraries and Qt framework/plugin files actually present in a package. When
available, the package includes the SBOM files exposed by the Qt installation
under `third-party/qt-installation-sboms/`, and the runtime inventory records
their hashes. This is an installation-level catalog, not a per-file mapping to
the shipped runtime. The Windows and macOS native packaging tests verify the
inventory and SBOM hashes. Linux system libraries remain owned by the host
distribution and are not represented as bundled files.

AxisSim's MIT license does not cover Qt or the Qt installation's third-party
components. The build environment uses the open-source Qt distribution and
the package includes the LGPLv3 text. Qt's own guidance describes
corresponding-source delivery or a written offer and, for dynamic linking,
relinking/install-information duties. The current package metadata and notices
do **not** claim those obligations are fully satisfied or that every shipped
runtime file has a resolved license mapping. Do not publish a release until the
project owner has reviewed the exact runtime inventory and SBOM catalog,
completed the applicable attribution/source review, and arranged any required
source or written-offer delivery and relinking information.

No signing credentials or notarization are configured. The macOS bundle is
ad-hoc signed for local validation. Windows/Linux/macOS validation artifacts
must be built and verified natively; a successful build on one OS is not
evidence for another.

An artifact marked `release` must name a real `refs/tags/<tag>` reference that
resolves to the packaged commit. This check prevents a branch with the same
name from being accepted as a tag. It does not verify a cryptographic tag
signature or authenticate who created the tag; release signing and trusted-tag
policy require a separate decision.

## Reproduction

```sh
./scripts/package_linux.sh build-package-check
```

```powershell
./scripts/package_windows.ps1 -BuildDir build-package-check
```

```sh
./scripts/package_macos.sh build-package-check
```

Each script prints its output paths and runs `tools/verify_extracted_package.py`
against a fresh extraction or relocation. The Windows and macOS packages
bundle Qt dynamically. Linux needs Qt development/runtime packages, `dpkg-dev`,
and the QPA plugins installed on the host. macOS packaging needs Qt 6,
`macdeployqt`, `codesign`, `hdiutil`, and a working Cocoa session for the
native GUI smoke test. The Windows static dependency check covers normal PE
imports; delay-load imports and dependencies opened through runtime
`LoadLibrary` calls still need separate analysis. Package-validation files are
checked and then discarded with their native CI runner workspaces; they are
not retained as downloadable workflow artifacts.

## Qt references

- [Qt open-source LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations)
- [Qt 6.8 third-party components and SBOM availability](https://doc.qt.io/qt-6.8/licenses-used-in-qt.html)
- [Qt software bill of materials](https://doc.qt.io/qt-6/sbom.html)
- [Qt 6 licensing](https://doc.qt.io/qt-6/licensing.html)
- [GNU LGPL version 3 text](https://www.gnu.org/licenses/lgpl-3.0.html)
