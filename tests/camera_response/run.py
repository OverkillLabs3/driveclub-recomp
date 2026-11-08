"""Run camera/HUD pure-core regression tests without game assets or a GPU.

Usage: python tests/camera_response/run.py --cc clang --output build/camera-tests
These tests cover scheduling, arithmetic, publication and lifecycle contracts.
They do not substitute for in-game validation of the native x86-64 hooks.
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent
    for path in sorted(source.glob("test_*.c")):
        exe = output / (path.stem + (".exe" if os.name == "nt" else ""))
        subprocess.run(
            [args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", str(path), "-o", str(exe)], check=True)
        print(path.name, flush=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
