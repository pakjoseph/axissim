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

The Qt installation SBOM used by the package build is included, when supplied
by the Qt installation, as `third-party/qt-installation-sbom.spdx`. It records
Qt and third-party package versions, copyright holders, license expressions,
and source references. It may describe components installed in the SDK that
are not shipped; consult the runtime inventory to identify the Qt binaries
actually included in that package. Qt documents that only third-party
components actually shipped with an application are relevant to its component
attribution list.

The bundled Qt runtime is dynamically linked and uses the open-source Qt
distribution. `licenses/LGPL-3.0.txt` is included for Qt components under
LGPLv3. The exact licenses of the Qt and third-party components must be read
from the package's Qt SBOM and the notices applicable to the platform bundle.
Do not infer a Qt license choice from the AxisSim MIT license.

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
