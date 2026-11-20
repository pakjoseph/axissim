# Package provenance and licensing

The native package scripts build and test validation artifacts on their target
operating systems. They do not publish or tag a release. The extracted package
contains the repository `LICENSE`, this project's third-party notice, Qt's
LGPLv3 license text, machine-readable package provenance, and a package-local
inventory of native runtime files and their SHA-256 values.

## Qt and other runtime dependencies

The AxisSim GUI links Qt 6 Core, Gui, and Widgets. Windows and macOS use
dynamically bundled Qt runtimes selected by `windeployqt` and `macdeployqt`.
Linux packages intentionally use system Qt libraries and platform plugins;
the portable tarball documents that dependency, and the Debian package uses
the shared-library dependency scan plus explicit dependencies for every
Qt runtime package resolved from the extracted binaries and the Qt QPA plugin package. The Linux
tarball is not a self-contained application bundle.

`third-party/qt-runtime-inventory.json` identifies the shared native libraries
and Qt framework/plugin files actually present in a package. When available,
`third-party/qt-installation-sbom.spdx` carries the SPDX SBOM from the build's
Qt installation. That SBOM can describe installed SDK packages not copied into
the app; use the runtime inventory to locate what the package actually ships.
The Windows and macOS native packaging tests verify the inventory hashes.
Linux system libraries remain owned by the host distribution and are not
represented as bundled files.

AxisSim's MIT license does not cover Qt or the Qt installation's third-party
components. The build environment uses the open-source Qt distribution and
the package includes the LGPLv3 text. Qt's own guidance describes
corresponding-source delivery or a written offer and, for dynamic linking,
relinking/install-information duties. The current package metadata and notices
do **not** claim those obligations are fully satisfied. Do not publish a
release until the project owner has reviewed the exact Qt SBOM/runtime
inventory and arranged any required source or written-offer delivery and
relinking information.

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
native GUI smoke test.

## Qt references

- [Qt open-source LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations)
- [Qt 6.8 third-party components and SBOM availability](https://doc.qt.io/qt-6.8/licenses-used-in-qt.html)
- [Qt software bill of materials](https://doc.qt.io/qt-6/sbom.html)
- [Qt 6 licensing](https://doc.qt.io/qt-6/licensing.html)
- [GNU LGPL version 3 text](https://www.gnu.org/licenses/lgpl-3.0.html)
