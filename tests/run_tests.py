"""Compile and run the portable core tests with GCC or Clang."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc" if os.name == "nt" else "cc"),
                        help="Path to a GCC-compatible compiler")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]

    with tempfile.TemporaryDirectory(prefix="error-log-tests-") as build_dir:
        for name, defines in (
            ("default", ()),
            ("one_slot", ("-DERROR_LOG_QUEUE_CAPACITY=1",)),
            ("disabled", ("-DERROR_LOG_ENABLED=0",)),
        ):
            suffix = ".exe" if os.name == "nt" else ""
            binary = Path(build_dir) / (name + suffix)
            subprocess.run(
                [args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 *defines, "-Iinclude", "src/error_log.c", "src/error_log_uart.c",
                 "tests/test_error_log.c", "-o", str(binary)],
                cwd=root,
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
