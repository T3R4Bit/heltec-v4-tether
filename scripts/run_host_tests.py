#!/usr/bin/env python3
"""Build and run TETHER's host unit tests (no hardware, no Meshtastic checkout needed).

Needs a C++17 compiler. Uses $CXX if set, else g++/clang++ on PATH, else `python -m ziglang c++`
(`pip install ziglang` gives a self-contained clang, handy on Windows).

Usage: python scripts/run_host_tests.py
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MODULE_DIR = REPO_ROOT / "module" / "Tether"
TEST_SOURCES = sorted((REPO_ROOT / "tests" / "host").glob("test_*.cpp"))


def find_compiler() -> list:
    if os.environ.get("CXX"):
        return os.environ["CXX"].split()
    for name in ("g++", "clang++"):
        if shutil.which(name):
            return [name]
    try:
        subprocess.run([sys.executable, "-m", "ziglang", "version"], check=True, capture_output=True)
        return [sys.executable, "-m", "ziglang", "c++"]
    except (subprocess.CalledProcessError, FileNotFoundError):
        sys.exit("run_host_tests: no C++ compiler found (set CXX, install g++/clang++, or `pip install ziglang`)")


def main() -> None:
    cxx = find_compiler()
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        for src in TEST_SOURCES:
            exe = Path(tmp) / (src.stem + (".exe" if os.name == "nt" else ""))
            cmd = cxx + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-I", str(MODULE_DIR), str(src), "-o", str(exe)]
            print("build:", src.relative_to(REPO_ROOT).as_posix(), flush=True)
            if subprocess.run(cmd).returncode != 0:
                failed = True
                continue
            print("run:  ", exe.name, flush=True)
            if subprocess.run([str(exe)]).returncode != 0:
                failed = True
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
