# Building

## Firmware version
TETHER builds against **Meshtastic `v2.8.2.d5f1619`**.

The `OPTIONAL_MODULES_SETUP` hook that TETHER relies on (`bin/optional-modules.py` + `Modules.cpp`) first
appears in **v2.8.1**. The latest *stable* release at the time of pinning (v2.7.26) does not have it, so the
pin is a 2.8.x pre-release. Do not drop below v2.8.1; `scripts/install_module.py` refuses older checkouts.

## Hook contract
`bin/optional-modules.py` runs before every build. For each `src/modules/optional/<Name>/` that holds
`<Name>.h`, it generates `$BUILD_DIR/OptionalModules.h` with an `#include` and a `setup<Name>()` call, and
`Modules.cpp` calls `OPTIONAL_MODULES_SETUP()`. Sources under the folder are compiled by the default
recursive source filter. So TETHER needs `module/Tether/Tether.h` declaring `void setupTether();`, and
no core file or `platformio.ini` edit.

## Steps
```sh
# 1. Firmware checkout. The path must not contain spaces (see below).
git clone --depth 1 --branch v2.8.2.d5f1619 --recurse-submodules --shallow-submodules \
    https://github.com/meshtastic/firmware.git <FIRMWARE_DIR>

# 2. Install the module (no-op error until module/Tether/Tether.h exists)
TETHER_FIRMWARE_DIR=<FIRMWARE_DIR> python scripts/install_module.py

# 3. Build
cd <FIRMWARE_DIR> && pio run -e heltec-v4
```
If the repo itself sits in a path without spaces you can use `./firmware` (gitignored) and skip
`TETHER_FIRMWARE_DIR`.

## Windows gotchas (all hit on 2026-11-05 while getting a stock build through)
| Symptom | Cause | Fix |
|---|---|---|
| `ERROR: MSys/Mingw is not supported` | Building from Git Bash | Build from PowerShell or cmd |
| `invalid argument '...' to '-fmacro-prefix-map'` | Space in the firmware path | Keep the firmware checkout in a path with no spaces |
| `'git init --bare' failed ... unable to get current working directory` | Microsoft Store Python redirects `%LOCALAPPDATA%` writes into its sandbox, so `git.exe` cannot see the IDF component cache | Set `IDF_COMPONENT_CACHE_PATH` to a normal folder outside `AppData` |
| `espressif32 is not compatible with PlatformIO Core v6.2.0-post.1` (and the platform gets uninstalled) | `pioarduino 6.2.0.post1` (PyPI, 2026-10-26) in the platform's penv reads its own version as a pre-release | `~/.platformio/penv/Scripts/uv.exe pip install --python ~/.platformio/penv/Scripts/python.exe pioarduino==6.2.0` |
| `Firmware metrics can not be shown` | Console code page | `chcp 65001` before building |

Optional: set `PLATFORMIO_WORKSPACE_DIR` to keep the ~1 GB `.pio` build folder out of OneDrive.

Working PowerShell environment used for the verified stock build:
```powershell
$env:PLATFORMIO_WORKSPACE_DIR = "$HOME\.tether-pio\workspace"
$env:IDF_COMPONENT_CACHE_PATH = "$HOME\.tether-pio\idf-component-cache"
chcp 65001
python -m platformio run -e heltec-v4
```

## Verified builds
| Date | Firmware | Env | Module | Result |
|---|---|---|---|---|
| 2026-11-05 | v2.8.2.d5f1619 | heltec-v4 | none (stock) | SUCCESS. Flash 35.0% (2,295,431 B), RAM 4.7% (98,480 B). Not flashed to a board. |
| 2026-11-05 | v2.8.2.d5f1619 | heltec-v4 | TETHER Phase 1 | SUCCESS. Hook printed `optional-modules: Tether`. Flash 35.1% (2,303,395 B, +7,964 B vs stock), static RAM 98,488 B (+8 B; the module's ~3 KB of history is heap-allocated at boot). Only warning is RadioLib's upstream `#warning` about USB CDC, also printed by the stock build. Not flashed to a board. |

## Host unit tests
`python scripts/run_host_tests.py` compiles `tests/host/*.cpp` against `module/Tether/` with any C++17 compiler
(`$CXX`, `g++`, `clang++`, or `pip install ziglang`) and runs them. They cover only the hardware-independent
headers (`TetherProtocol.h`, `TetherMath.h`, `TetherStats.h`), not the Meshtastic-facing `TetherModule.cpp`.
