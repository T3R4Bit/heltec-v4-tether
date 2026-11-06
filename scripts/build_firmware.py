#!/usr/bin/env python3
"""One command to build (and optionally flash) Meshtastic + TETHER for the Heltec V4.

    python scripts/build_firmware.py                       # clone firmware if needed, install module, build
    python scripts/build_firmware.py --upload --port COM5  # ...and flash a board (Linux/macOS: /dev/ttyUSB0 etc.)
    python scripts/build_firmware.py --layout 2            # pick the OLED layout (docs/OLED.md)

Firmware checkout: $TETHER_FIRMWARE_DIR if set, else ./firmware, else (when this repo's path contains a space, which
ESP-IDF cannot build from) ~/tether-firmware. It is cloned at the pinned version in scripts/FIRMWARE_VERSION.
On Windows this applies the workarounds from docs/BUILD.md automatically (run it from PowerShell or cmd, not Git Bash).
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VERSION = (REPO / "scripts" / "FIRMWARE_VERSION").read_text().strip()
ENV = "heltec-v4"


def firmware_dir() -> Path:
    if os.environ.get("TETHER_FIRMWARE_DIR"):
        return Path(os.environ["TETHER_FIRMWARE_DIR"]).expanduser().resolve()
    local = REPO / "firmware"
    if " " not in str(local):
        return local
    return Path.home() / "tether-firmware"


def run(cmd, **kw):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode != 0:
        sys.exit("build_firmware: command failed (exit %d)" % r.returncode)
    return r


def ensure_checkout(fw: Path) -> None:
    if not fw.exists():
        run(["git", "-c", "core.longpaths=true", "-c", "advice.detachedHead=false", "clone", "--depth", "1", "--branch",
             VERSION, "--recurse-submodules", "--shallow-submodules", "https://github.com/meshtastic/firmware.git", fw])
        return
    tag = subprocess.run(["git", "-C", str(fw), "describe", "--tags"], capture_output=True, text=True).stdout.strip()
    if tag != VERSION:
        print("build_firmware: warning: %s is at %r, pinned version is %s" % (fw, tag or "?", VERSION), file=sys.stderr)


def build_env() -> dict:
    env = dict(os.environ)
    if os.name == "nt":
        if env.get("MSYSTEM"):
            sys.exit("build_firmware: ESP-IDF refuses to run under Git Bash/MSYS. Run this from PowerShell or cmd.")
        base = Path.home() / ".tether-pio"
        env.setdefault("PLATFORMIO_WORKSPACE_DIR", str(base / "workspace"))      # keep ~1 GB out of OneDrive
        env.setdefault("IDF_COMPONENT_CACHE_PATH", str(base / "idf-component-cache"))  # Store Python sandbox fix
        Path(env["IDF_COMPONENT_CACHE_PATH"]).mkdir(parents=True, exist_ok=True)
        env.setdefault("PYTHONIOENCODING", "utf-8")
    return env


def pin_pioarduino(env: dict) -> None:
    """pioarduino 6.2.0.post1 rejects its own espressif32 platform (docs/BUILD.md); pin 6.2.0 in the platform penv."""
    penv = Path.home() / ".platformio" / "penv"
    scripts = penv / ("Scripts" if os.name == "nt" else "bin")
    py, uv = scripts / ("python.exe" if os.name == "nt" else "python"), scripts / ("uv.exe" if os.name == "nt" else "uv")
    if not py.exists():
        return  # created on the first build; the pin is applied on the next run if that first build trips over it
    out = subprocess.run([str(py), "-c", "import platformio; print(platformio.__version__)"], capture_output=True,
                         text=True, env=env).stdout.strip()
    if out == "6.2.0.post1" and uv.exists():
        run([uv, "pip", "install", "--python", py, "pioarduino==6.2.0"], env=env)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--upload", action="store_true", help="flash the board after building")
    ap.add_argument("--port", help="serial port for --upload (default: PlatformIO auto-detect)")
    ap.add_argument("--layout", type=int, choices=(1, 2, 3), help="OLED layout, see docs/OLED.md")
    ap.add_argument("--peer", help="hard-code the peer node number, e.g. 0x1a2b3c4d (default: the favorited node)")
    args = ap.parse_args()

    env = build_env()  # fails fast under Git Bash on Windows
    fw = firmware_dir()
    if " " in str(fw):
        sys.exit("build_firmware: %s contains a space; ESP-IDF cannot build there. Set TETHER_FIRMWARE_DIR." % fw)
    ensure_checkout(fw)
    run([sys.executable, REPO / "scripts" / "install_module.py", "--firmware", fw])

    flags = []
    if args.layout:
        flags.append("-DTETHER_OLED_LAYOUT=%d" % args.layout)
    if args.peer:
        flags.append("-DTETHER_PEER_NODENUM=%s" % args.peer)
    if flags:
        # PLATFORMIO_BUILD_FLAGS is appended to every env's build_flags; no platformio.ini edit needed.
        env["PLATFORMIO_BUILD_FLAGS"] = " ".join(flags)

    pio = [sys.executable, "-m", "platformio"]
    if not shutil.which("pio") and subprocess.run(pio + ["--version"], capture_output=True).returncode != 0:
        sys.exit("build_firmware: PlatformIO not found: python -m pip install platformio")
    pin_pioarduino(env)
    cmd = pio + ["run", "-e", ENV]
    if args.upload:
        cmd += ["-t", "upload"]
        if args.port:
            cmd += ["--upload-port", args.port]
    run(cmd, cwd=fw, env=env)

    build_dir = Path(env.get("PLATFORMIO_WORKSPACE_DIR", fw / ".pio")) / "build" / ENV
    for f in sorted(build_dir.glob("firmware-*.bin")):
        print("build_firmware: %s" % f)


if __name__ == "__main__":
    main()
