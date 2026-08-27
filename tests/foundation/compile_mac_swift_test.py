#!/usr/bin/env python3
"""Unit tests for the product-owned macOS Swift compiler wrapper."""

from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path


sys.dont_write_bytecode = True
MODULE_PATH = (
    Path(__file__).resolve().parents[2] / "tools" / "build" / "compile_mac_swift.py"
)
SPEC = importlib.util.spec_from_file_location(
    "limitless_compile_mac_swift", MODULE_PATH
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"could not load {MODULE_PATH}")
COMPILE_MAC_SWIFT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPILE_MAC_SWIFT)


class CompileMacSwiftTest(unittest.TestCase):
    def test_generated_header_gets_a_nonmodule_framework_fallback(self) -> None:
        with tempfile.TemporaryDirectory(prefix="limitless-swift-header-test-") as root:
            source = Path(root) / "raw.h"
            destination = Path(root) / "generated.h"
            source.write_text(
                """before
#if __has_feature(objc_modules)
# if __has_warning(\"-Watimport-in-framework-header\")
#pragma clang diagnostic ignored \"-Watimport-in-framework-header\"
# endif
# ifdef LIMITLESS_INCLUDE_FOUNDATION
@import Foundation;
# endif
@import AppKit;
@import ObjectiveC;
#endif
after
""",
                encoding="utf-8",
            )

            COMPILE_MAC_SWIFT.rewrite_generated_header(source, destination)

            self.assertEqual(
                """before
#if __has_feature(objc_modules)
# if __has_warning(\"-Watimport-in-framework-header\")
#pragma clang diagnostic ignored \"-Watimport-in-framework-header\"
# endif
# ifdef LIMITLESS_INCLUDE_FOUNDATION
@import Foundation;
# endif
@import AppKit;
@import ObjectiveC;
#else
# if __has_warning(\"-Watimport-in-framework-header\")
#pragma clang diagnostic ignored \"-Watimport-in-framework-header\"
# endif
# ifdef LIMITLESS_INCLUDE_FOUNDATION
#import <Foundation/Foundation.h>
# endif
#import <AppKit/AppKit.h>
#endif
after
""",
                destination.read_text(encoding="utf-8"),
            )

    def test_unchanged_header_preserves_timestamp(self) -> None:
        with tempfile.TemporaryDirectory(prefix="limitless-swift-header-test-") as root:
            source = Path(root) / "raw.h"
            destination = Path(root) / "generated.h"
            source.write_text("plain header\n", encoding="utf-8")
            COMPILE_MAC_SWIFT.rewrite_generated_header(source, destination)
            stable_timestamp = 1_700_000_000_000_000_000
            os.utime(destination, ns=(stable_timestamp, stable_timestamp))

            COMPILE_MAC_SWIFT.rewrite_generated_header(source, destination)

            self.assertEqual(stable_timestamp, destination.stat().st_mtime_ns)

    def test_malformed_import_sections_fail_closed(self) -> None:
        cases = (
            "#if __has_feature(objc_modules)\n@import AppKit;\n",
            "#if __has_feature(objc_modules)\n#else\n#endif\n",
            "#if __has_feature(objc_modules)\n@import AppKit.Private;\n#endif\n",
        )
        for contents in cases:
            with self.subTest(contents=contents):
                with tempfile.TemporaryDirectory(
                    prefix="limitless-swift-header-test-"
                ) as root:
                    source = Path(root) / "raw.h"
                    destination = Path(root) / "generated.h"
                    source.write_text(contents, encoding="utf-8")

                    with self.assertRaises(RuntimeError):
                        COMPILE_MAC_SWIFT.rewrite_generated_header(source, destination)
                    self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
