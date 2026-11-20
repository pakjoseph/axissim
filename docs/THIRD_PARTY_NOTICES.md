# AxisSim package notices

AxisSim application code is distributed under the MIT License. A copy is
included as `LICENSE` in portable packages and in the package documentation
directory for native Linux packages.

## Qt 6 runtime

The desktop application uses Qt 6 Core, Gui, and Widgets. Windows and macOS
portable packages dynamically bundle the Qt libraries and platform plugin
files selected by `windeployqt` or `macdeployqt`. Linux archives and Debian
packages rely on system Qt runtime libraries; the package metadata and native
package dependency list identify that distinction. The exact files found in a
particular package are listed in `third-party/qt-runtime-inventory.json`.

The Qt installation SBOM files available to the package build are copied under
`third-party/qt-installation-sboms/`. The adjacent
`third-party/qt-runtime-inventory.json` hashes that catalog and separately
lists the exact native runtime files in the package. The SBOMs describe Qt
installation components and may include modules that are not shipped; they are
not asserted to map one-to-one to the runtime files. Review both inventories
before making a release attribution or licensing conclusion. Qt documents that
only third-party components actually shipped with an application are relevant
to its component attribution list.

The bundled Qt runtime is dynamically linked and uses the open-source Qt
distribution. `licenses/LGPL-3.0.txt` is included for Qt components under
LGPLv3. The SBOM catalog and the exact native-file inventory provide evidence
for review, but this package does not yet prove a complete per-runtime-file
license mapping. Do not infer a Qt license choice from the AxisSim MIT license.

The Windows package also includes the Microsoft `vc_redist.x64.exe` installer
selected by `windeployqt --compiler-runtime`; its path and SHA-256 are recorded
as a separate prerequisite in `third-party/qt-runtime-inventory.json`. The ZIP
does not carry app-local MSVC runtime DLLs. Run the included redistributable
installer if the Microsoft Visual C++ Redistributable (x64, 14.x) is not already
installed on the target system. Redistribution is subject to the applicable
Microsoft Visual Studio license terms; see the Microsoft reference below.

## Distribution boundary

These files improve package attribution and license visibility. They do not by
themselves establish that every applicable source-code, relinking, or
installation-information obligation has been met. Qt's LGPL guidance describes
corresponding-source delivery or a written offer, and additional requirements
for relinking. The current packages are validation artifacts only; do not
publish them as a release until the project owner has reviewed and satisfied
those obligations for the exact runtime inventory.

Official references:

- Qt open-source LGPL obligations: <https://www.qt.io/development/open-source-lgpl-obligations>
- Qt 6.8 third-party code list and SBOM availability: <https://doc.qt.io/qt-6.8/licenses-used-in-qt.html>
- Qt software bill of materials: <https://doc.qt.io/qt-6/sbom.html>
- GNU LGPL version 3 text: <https://www.gnu.org/licenses/lgpl-3.0.html>
- Microsoft Visual C++ redistributable files: <https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170>
