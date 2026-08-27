#!/usr/bin/env python3
# Copyright 2026 The Limitless Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Compile one product-owned macOS Swift module without patching GN tools."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


IMPORT_SECTION_START = re.compile(
    r"^\s*#\s*if\s+__has_feature\s*\(\s*objc_modules\s*\)\s*$"
)
PREPROCESSOR_DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")
MODULE_IMPORT = re.compile(r"^@import\s+([A-Za-z_][A-Za-z0-9_]*)\s*;$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--swiftc", required=True)
    parser.add_argument("--sdk", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--module-name", required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--object", type=Path, required=True)
    parser.add_argument("--swiftmodule", type=Path, required=True)
    parser.add_argument("--xcode-build", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--debug", action="store_true")
    mode.add_argument("--optimize", action="store_true")
    parser.add_argument("source", type=Path)
    return parser.parse_args()


def write_if_changed(path: Path, content: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def rewrite_generated_header(source: Path, destination: Path) -> None:
    """Adds non-module imports while preserving Swift's module-enabled path."""
    output: list[str] = []
    imports_section: tuple[int, int] | None = None
    for line in source.read_text(encoding="utf-8").splitlines(keepends=True):
        if imports_section is None and IMPORT_SECTION_START.match(line):
            imports_section = (len(output) + 1, 1)
        elif imports_section is not None:
            section_start, nesting_level = imports_section
            directive_match = PREPROCESSOR_DIRECTIVE.match(line)
            directive = directive_match.group(1) if directive_match else None
            if directive in ("if", "ifdef", "ifndef"):
                imports_section = (section_start, nesting_level + 1)
            elif nesting_level == 1 and directive in ("else", "elif"):
                raise RuntimeError(
                    "generated Swift import section already has a fallback branch"
                )
            elif directive == "endif":
                if nesting_level > 1:
                    imports_section = (section_start, nesting_level - 1)
                else:
                    imports_section = None
                    section_end = len(output)
                    output.append("#else\n")
                    for import_line in output[section_start:section_end]:
                        stripped_import = import_line.strip()
                        if stripped_import.startswith("@import"):
                            import_match = MODULE_IMPORT.fullmatch(stripped_import)
                            if import_match is None:
                                raise RuntimeError(
                                    "unsupported generated Swift module import: "
                                    f"{stripped_import}"
                                )
                            framework = import_match.group(1)
                            if framework != "ObjectiveC":
                                output.append(f"#import <{framework}/{framework}.h>\n")
                        else:
                            output.append(import_line)
        output.append(line)
    if imports_section is not None:
        raise RuntimeError("unterminated generated Swift import section")
    write_if_changed(destination, "".join(output))


def main() -> int:
    args = parse_args()
    for output in (args.header, args.object, args.swiftmodule):
        output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="limitless-swift-header-") as temp:
        raw_header = Path(temp) / args.header.name
        command = [
            args.swiftc,
            "-parse-as-library",
            "-emit-object",
            "-emit-module",
            "-emit-module-path",
            str(args.swiftmodule),
            "-emit-objc-header",
            "-emit-objc-header-path",
            str(raw_header),
            "-module-name",
            args.module_name,
            "-target",
            args.target,
            "-sdk",
            args.sdk,
            "-swift-version",
            "6",
            "-Onone" if args.debug else "-O",
            "-g" if args.debug else "-whole-module-optimization",
            "-o",
            str(args.object),
            str(args.source),
        ]
        subprocess.run(command, check=True)
        rewrite_generated_header(raw_header, args.header)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
