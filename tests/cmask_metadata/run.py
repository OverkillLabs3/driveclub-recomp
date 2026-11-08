"""Run CMask upload-precedence regressions without game assets or a GPU.

Usage: python tests/cmask_metadata/run.py --cxx clang++ --output build/cmask-tests
The negative control reproduces a naive recursive texture-cache lock and
must time out; the production leaf-mutex path must finish under the same locks.
"""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent
    root = source.parent.parent
    exe = output / ("test_metadata.exe" if os.name == "nt" else "test_metadata")
    subprocess.run(
        [args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-pthread", "-I" + str(root / "gpu/shadps4"),
         str(source / "test_metadata.cpp"), "-o", str(exe)],
        check=True,
    )
    subprocess.run([str(exe)], check=True, timeout=10)
    try:
        subprocess.run([str(exe), "recursive-lock-negative-control"], check=True, timeout=2)
    except subprocess.TimeoutExpired:
        print("PASS: recursive-lock negative control timed out", flush=True)
    else:
        raise AssertionError("Recursive-lock negative control unexpectedly completed")


if __name__ == "__main__":
    main()
