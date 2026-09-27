# SPDX-FileCopyrightText: 2025 Authors (see AUTHORS.txt)
#
# SPDX-License-Identifier: Apache-2.0

"""Config command.

This command has also been extended to export compile commands for CMake
(`compile_commands.json` file), mainly for syntaxic checkers.
"""

import os
import shutil

from .constants import PARAMS
from .utils import run_cmake, fail, logger


def config(
    _,  # args (unused)
):
    """CMake config."""
    # Check whether presets exist
    presets = PARAMS.BINARY_DIR / "build" / "generators" / "CMakePresets.json"
    if not presets.exists():
        fail(
            "Cannot find presets file ('%s'). "
            "Have you run 'make deps' beforehand?",
            str(presets.absolute()),
        )

    # Prepare and run command
    cmd = [
        "--preset conan-default",
        f"-DCMAKE_INSTALL_PREFIX={str(PARAMS.INSTALL_DIR)}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-S {str(PARAMS.SOURCE_DIR)}",
    ]

    # Compiler cache (opt-in via SUPERLUXCORE_CCACHE=1): only useful for
    # clean/branch-switch rebuilds — a touched header changes every
    # dependent TU's preprocessed hash, so incremental builds always
    # miss. Moreover CMake emits PCH via `-Xclang -include-pch/-pth`,
    # which sccache 0.17 cannot cache (pass-through for all PCH TUs), so
    # under the PCH build it is a no-op with per-invocation overhead.
    if os.environ.get("SUPERLUXCORE_CCACHE"):
        launcher = next(
            (x for x in ("sccache", "ccache") if shutil.which(x)), None
        )
        if launcher:
            cmd += [
                f"-DCMAKE_C_COMPILER_LAUNCHER={launcher}",
                f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}",
            ]
            logger.info("Compiler launcher: %s", launcher)
        else:
            logger.warning(
                "SUPERLUXCORE_CCACHE set but no sccache/ccache in PATH"
            )

    run_cmake(cmd)

    # Info
    compile_commands_file = (
        PARAMS.BINARY_DIR / "build" / "compile_commands.json"
    )
    logger.info(
        "Compile commands file generated at: '%s'", compile_commands_file
    )
