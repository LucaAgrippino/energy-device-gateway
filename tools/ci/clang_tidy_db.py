#!/usr/bin/env python3
"""Rewrite compile_commands.json so esp-clang's clang-tidy can consume it.

Run clang-tidy with Espressif's LLVM build (`idf_tools.py install esp-clang`),
not a stock one: stock clang has no Xtensa backend, rejects -mlongcalls, and
cannot parse ESP-IDF headers for a 32-bit target from an x86-64 host. Trying to
paper over that with include shims trades one breakage for another.

Even esp-clang needs two adjustments, both applied here:

  * ESP-IDF compiles with GCC, and a handful of its flags have no clang
    spelling at all. clang reports them as clang-diagnostic-error, which under
    WarningsAsErrors: '*' fails the run before a single check executes.
  * The C++ standard library headers live in a multilib tree, so the target
    variant holding bits/c++config.h has to be named explicitly.

The original compile_commands.json is left untouched so it keeps working for
clangd and idf.py.

    python3 tools/ci/clang_tidy_db.py build build/tidy
    clang-tidy -p build/tidy <sources>
"""

from __future__ import annotations

import json
import os
import shlex
import sys

# GCC-only optimiser flags. clang has no equivalent and rejects them outright.
DROP = {
    "-fno-shrink-wrap",
    "-fno-tree-switch-conversion",
    "-fstrict-volatile-bitfields",
    "-fno-inline-functions-called-once",
    "-fno-inline-small-functions",
}

# GCC-only diagnostics. Harmless, but each would draw -Wunknown-warning-option.
DROP_PREFIX = (
    "-Wformat-overflow",
    "-Wformat-truncation",
    "-Wformat-signedness",
    "-Wlogical-op",
    "-Wno-old-style-declaration",
    "-Wuse-after-free",
    "-fdiagnostics-color",
)

TARGET = "xtensa-esp-elf"
CHIP = "esp32s3"

# Appended AFTER every flag ESP-IDF supplies. -W options are last-wins, so a
# suppression placed early gets undone by the -Wall/-Werror that follow it.
SUPPRESS = [
    # -mlongcalls is accepted but unused when clang is only parsing, and
    # WarningsAsErrors: '*' would promote that note into a failure.
    "-Wno-unused-command-line-argument",
    # ESP-IDF's i2c_types.h declares an empty struct inside extern "C".
    # Real, harmless, and not this project's to fix.
    "-Wno-extern-c-compat",
]


def keep(flag: str) -> bool:
    return flag not in DROP and not flag.startswith(DROP_PREFIX)


def target_args(compiler: str) -> list[str]:
    """Point clang at the GCC toolchain that owns the headers and the target.

    Derived from the compiler recorded in the compile database rather than
    hard-coded, since the toolchain sits under ~/.espressif locally and under
    /opt/esp in the CI image.
    """
    root = os.path.dirname(os.path.dirname(os.path.abspath(compiler)))
    sysroot = os.path.join(root, TARGET)
    args = [f"--target={TARGET}", f"--sysroot={sysroot}"]

    # bits/c++config.h is per-multilib; without the chip-specific directory
    # every <cstdint> include fails to resolve.
    cxx_root = os.path.join(sysroot, "include", "c++")
    if os.path.isdir(cxx_root):
        for version in sorted(os.listdir(cxx_root)):
            variant = os.path.join(cxx_root, version, TARGET, CHIP)
            if os.path.isdir(variant):
                args += ["-isystem", variant]
                break
    return args


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    build_dir, out_dir = sys.argv[1], sys.argv[2]

    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as fh:
        entries = json.load(fh)

    cache: dict[str, list[str]] = {}
    for entry in entries:
        tokens = [t for t in shlex.split(entry["command"]) if keep(t)]
        compiler = tokens[0]
        if compiler not in cache:
            cache[compiler] = target_args(compiler)
        # Target and sysroot go after argv[0] so they cannot be mistaken for the
        # output file; the -Wno suppressions go last so nothing re-enables them.
        tokens[1:1] = cache[compiler]
        tokens += SUPPRESS
        entry["command"] = shlex.join(tokens)

    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, "compile_commands.json")
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(entries, fh, indent=1)

    print(f"wrote {dst} ({len(entries)} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
