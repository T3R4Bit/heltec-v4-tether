#!/usr/bin/env python3
"""Copy module/Tether/ into a Meshtastic firmware checkout's src/modules/optional/.

Meshtastic (v2.8.1 and later) runs bin/optional-modules.py before each build. It finds every
src/modules/optional/<Name>/<Name>.h, includes it, and calls setup<Name>() from Modules.cpp through
the OPTIONAL_MODULES_SETUP hook. So installing TETHER is just a copy: no core file is patched and no
platformio.ini edit is needed.

Usage (from the repo root):
    python scripts/install_module.py                 # install into $TETHER_FIRMWARE_DIR, else ./firmware
    python scripts/install_module.py --firmware PATH # install into another checkout
    python scripts/install_module.py --remove        # uninstall (back to a stock build)

ESP-IDF cannot build from a path that contains spaces. If this repo lives in one, keep the firmware
checkout elsewhere and point TETHER_FIRMWARE_DIR (or --firmware) at it.
"""

import argparse
import os
import shutil
import sys
from pathlib import Path

MODULE_NAME = "Tether"
REPO_ROOT = Path(__file__).resolve().parent.parent
MODULE_SRC = REPO_ROOT / "module" / MODULE_NAME
IGNORED = shutil.ignore_patterns(".gitkeep", ".DS_Store", "*.orig", "*.rej")


def fail(message: str) -> None:
    print(f"install_module: error: {message}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    default_firmware = Path(os.environ["TETHER_FIRMWARE_DIR"]) if os.environ.get("TETHER_FIRMWARE_DIR") else REPO_ROOT / "firmware"
    parser.add_argument("--firmware", type=Path, default=default_firmware, help="Meshtastic firmware checkout (default: $TETHER_FIRMWARE_DIR, else ./firmware)")
    parser.add_argument("--remove", action="store_true", help="remove the installed module instead of installing it")
    args = parser.parse_args()

    firmware = args.firmware.resolve()
    if " " in str(firmware):
        print(f"install_module: warning: {firmware} contains a space; ESP-IDF builds fail from such paths", file=sys.stderr)
    if not (firmware / "platformio.ini").is_file() or not (firmware / "src" / "modules").is_dir():
        fail(f"{firmware} does not look like a Meshtastic firmware checkout")
    if not (firmware / "bin" / "optional-modules.py").is_file():
        fail(f"{firmware} has no bin/optional-modules.py, so it lacks the OPTIONAL_MODULES_SETUP hook. Use Meshtastic v2.8.1 or later.")

    dest = firmware / "src" / "modules" / "optional" / MODULE_NAME

    if args.remove:
        if dest.exists():
            shutil.rmtree(dest)
            print(f"install_module: removed {dest}")
        else:
            print(f"install_module: nothing installed at {dest}")
        return

    if not (MODULE_SRC / f"{MODULE_NAME}.h").is_file():
        fail(f"{MODULE_SRC / (MODULE_NAME + '.h')} is missing. The build hook skips a module without it, so there is nothing to install yet.")

    # Replace rather than merge, so files deleted from module/Tether/ do not linger in the checkout.
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(MODULE_SRC, dest, ignore=IGNORED)

    copied = sorted(p.relative_to(dest).as_posix() for p in dest.rglob("*") if p.is_file())
    print(f"install_module: installed {len(copied)} file(s) into {dest}")
    for name in copied:
        print(f"  {name}")


if __name__ == "__main__":
    main()
