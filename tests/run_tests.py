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
    core_test = "tests/test_error_log.c"
    flash_test = "tests/test_error_log_flash.c"
    sources = ["src/error_log.c", "src/error_log_uart.c", "src/error_log_flash.c"]

    with tempfile.TemporaryDirectory(prefix="error-log-tests-") as build_dir:
        for name, test, defines in (
            ("default", core_test, ()),
            ("one_slot", core_test, ("-DERROR_LOG_QUEUE_CAPACITY=1",)),
            ("disabled", core_test, ("-DERROR_LOG_ENABLED=0",)),
            ("disabled_with_flash", core_test,
             ("-DERROR_LOG_ENABLED=0", "-DERROR_LOG_FLASH_ENABLED=1")),
            ("flash", flash_test, ("-DERROR_LOG_FLASH_ENABLED=1",)),
        ):
            suffix = ".exe" if os.name == "nt" else ""
            binary = Path(build_dir) / (name + suffix)
            subprocess.run(
                [args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 *defines, "-Iinclude", *sources, test, "-o", str(binary)],
                cwd=root,
                check=True,
            )
            print(f"Running {name}", flush=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
