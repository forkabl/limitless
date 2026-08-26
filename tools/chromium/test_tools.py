#!/usr/bin/env python3
"""Focused contract tests for the Chromium workflow tools.

Every test creates a tiny sibling Limitless and Chromium workspace. The real
Chromium checkout is never used or modified.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PATCH_NAME = "0001-webui-browser-limitless-resources.patch"
UPSTREAM_FILE = "chrome/browser/ui/webui_browser/BUILD.gn"


def invoke(
    argv: list[str | Path],
    *,
    cwd: Path,
    check: bool = True,
    suppress_bytecode: bool = True,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    if suppress_bytecode:
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
    else:
        environment.pop("PYTHONDONTWRITEBYTECODE", None)
    completed = subprocess.run(
        [os.fspath(item) for item in argv],
        cwd=cwd,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if check and completed.returncode != 0:
        raise AssertionError(
            f"Command failed: {argv}\nstdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    return completed


def write_executable(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    path.chmod(0o755)


class ToolFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="limitless-tool-test-")
        self.workspace = Path(self.temporary.name)
        self.limitless = self.workspace / "limitless"
        self.chromium = self.workspace / "chromium" / "src"
        self.tools = self.limitless / "tools" / "chromium"
        self.chromium.mkdir(parents=True)

        source_tools = Path(__file__).resolve().parent
        shutil.copytree(
            source_tools,
            self.tools,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )

        invoke(["git", "init", "-q"], cwd=self.chromium)
        invoke(
            ["git", "config", "user.email", "fixture@example.invalid"],
            cwd=self.chromium,
        )
        invoke(["git", "config", "user.name", "Fixture"], cwd=self.chromium)
        (self.chromium / ".gitignore").write_text("/out/\n", encoding="utf-8")
        upstream = self.chromium / UPSTREAM_FILE
        upstream.parent.mkdir(parents=True)
        original = 'source_set("impl") {\n  sources = []\n}\n'
        upstream.write_text(original, encoding="utf-8")
        invoke(["git", "add", "."], cwd=self.chromium)
        invoke(["git", "commit", "-qm", "fixture base"], cwd=self.chromium)
        self.pin = invoke(
            ["git", "rev-parse", "HEAD"], cwd=self.chromium
        ).stdout.strip()

        upstream.write_text(
            'source_set("impl") {\n  defines = [ "LIMITLESS_BUILD" ]\n'
            "  sources = []\n}\n",
            encoding="utf-8",
        )
        patch_bytes = invoke(
            ["git", "diff", "--binary", "--", UPSTREAM_FILE], cwd=self.chromium
        ).stdout.encode()
        upstream.write_text(original, encoding="utf-8")

        patches = self.limitless / "patches" / "chromium"
        patches.mkdir(parents=True)
        (patches / PATCH_NAME).write_bytes(patch_bytes)
        (patches / "series").write_text(PATCH_NAME + "\n", encoding="utf-8")
        manifest = {
            "schema_version": 1,
            "chromium_base_sha": self.pin,
            "patches": [
                {
                    "file": PATCH_NAME,
                    "sha256": hashlib.sha256(patch_bytes).hexdigest(),
                    "upstream_files": [UPSTREAM_FILE],
                    "upstream_symbols": ['source_set("impl")'],
                    "limitless_targets": ["$limitless_root/build/chromium:resources"],
                    "tests": ["//chrome/test:browser_tests"],
                    "conflict_signals": ["impl target renamed"],
                    "removal_condition": "Chromium exposes an external resource seam.",
                }
            ],
        }
        (patches / "manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        (self.limitless / "chromium.version").write_text(
            self.pin + "\n", encoding="utf-8"
        )
        branding = self.limitless / "branding" / "BRANDING"
        branding.parent.mkdir(parents=True)
        branding.write_text("PRODUCT_FULLNAME=Limitless\n", encoding="utf-8")
        external_build = self.limitless / "build" / "chromium" / "BUILD.gn"
        external_build.parent.mkdir(parents=True)
        external_build.write_text('group("resources") {}\n', encoding="utf-8")

        fake_gn = self.chromium / "buildtools" / "mac" / "gn"
        write_executable(
            fake_gn,
            """#!/usr/bin/env python3
import pathlib
import sys
if sys.argv[1:3] == ["format", "--stdin"]:
    sys.stdout.write(sys.stdin.read())
elif len(sys.argv) >= 3 and sys.argv[1] == "gen":
    output = pathlib.Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    (output / "build.ninja").write_text("# fixture\\n", encoding="utf-8")
else:
    raise SystemExit(3)
""",
        )
        with (self.chromium / ".git" / "info" / "exclude").open(
            "a", encoding="utf-8"
        ) as excludes:
            excludes.write("/buildtools/mac/gn\n")
        write_executable(
            self.workspace / "depot_tools" / "autoninja",
            "#!/bin/sh\nexit 0\n",
        )

        invoke(["git", "init", "-q"], cwd=self.limitless)
        invoke(
            ["git", "config", "user.email", "fixture@example.invalid"],
            cwd=self.limitless,
        )
        invoke(["git", "config", "user.name", "Fixture"], cwd=self.limitless)
        invoke(["git", "add", "."], cwd=self.limitless)
        invoke(["git", "commit", "-qm", "fixture Limitless"], cwd=self.limitless)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def tool(
        self, name: str, *arguments: str, check: bool = True
    ) -> subprocess.CompletedProcess[str]:
        return invoke([self.tools / name, *arguments], cwd=self.workspace, check=check)

    def apply(self) -> None:
        completed = self.tool("apply-patches")
        self.assertIn("Patch series applied and verified exactly", completed.stdout)

    def generate(self) -> None:
        completed = self.tool("gen")
        self.assertIn("Generated and checked", completed.stdout)


class VerificationTests(ToolFixture):
    def test_entry_point_does_not_write_bytecode(self) -> None:
        cache = self.tools / "__pycache__"
        completed = invoke(
            [self.tools / "verify", "--json"],
            cwd=self.workspace,
            suppress_bytecode=False,
        )
        self.assertEqual(0, completed.returncode)
        self.assertFalse(cache.exists())

    def test_verify_reports_clean_json_and_validates_hashes(self) -> None:
        completed = self.tool("verify", "--json")
        payload = json.loads(completed.stdout)
        self.assertEqual("clean", payload["chromium_state"])
        self.assertEqual(self.pin, payload["pin"])
        self.assertEqual(1, payload["patch_count"])

    def test_hash_mismatch_is_actionable(self) -> None:
        patch = self.limitless / "patches" / "chromium" / PATCH_NAME
        patch.write_bytes(patch.read_bytes() + b"\n")
        completed = self.tool("verify", check=False)
        self.assertEqual(2, completed.returncode)
        self.assertIn("Patch hash mismatch", completed.stderr)

    def test_unrelated_chromium_change_is_rejected(self) -> None:
        (self.chromium / "unrelated.txt").write_text("not a patch\n", encoding="utf-8")
        completed = self.tool("verify", check=False)
        self.assertEqual(2, completed.returncode)
        self.assertIn(
            "neither clean nor the exact recorded patch series", completed.stderr
        )
        self.assertIn("unrelated.txt", completed.stderr)

    def test_change_on_recorded_path_must_match_exact_blob(self) -> None:
        self.apply()
        (self.chromium / UPSTREAM_FILE).write_text("wrong\n", encoding="utf-8")
        completed = self.tool("verify", check=False)
        self.assertEqual(2, completed.returncode)
        self.assertIn("do not exactly match the patch series", completed.stderr)


class PatchTests(ToolFixture):
    def test_check_rejects_silent_offset_application(self) -> None:
        patch = self.limitless / "patches" / "chromium" / PATCH_NAME
        patch_text = patch.read_text(encoding="utf-8")
        original_header = "@@ -1,3 +1,4 @@"
        shifted_header = "@@ -2,3 +2,4 @@"
        self.assertIn(original_header, patch_text)
        patch.write_text(
            patch_text.replace(original_header, shifted_header, 1),
            encoding="utf-8",
        )

        manifest_path = self.limitless / "patches" / "chromium" / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["patches"][0]["sha256"] = hashlib.sha256(
            patch.read_bytes()
        ).hexdigest()
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )

        completed = self.tool("apply-patches", "--check", check=False)
        self.assertEqual(2, completed.returncode)
        self.assertIn("requires an offset or fuzz", completed.stderr)
        self.assertIn("offset -1", completed.stderr)
        self.assertEqual(
            "", invoke(["git", "status", "--short"], cwd=self.chromium).stdout
        )

    def test_check_does_not_modify_chromium(self) -> None:
        before = invoke(["git", "status", "--short"], cwd=self.chromium).stdout
        completed = self.tool("apply-patches", "--check")
        after = invoke(["git", "status", "--short"], cwd=self.chromium).stdout
        self.assertEqual(before, after)
        self.assertIn("Chromium was not modified", completed.stdout)

    def test_apply_produces_exact_known_state(self) -> None:
        self.apply()
        completed = self.tool("verify", "--json")
        self.assertEqual("patched", json.loads(completed.stdout)["chromium_state"])

    def test_check_refuses_already_patched_state(self) -> None:
        self.apply()
        completed = self.tool("apply-patches", "--check", check=False)
        self.assertEqual(2, completed.returncode)
        self.assertIn("requires a clean Chromium checkout", completed.stderr)


class DryRunTests(ToolFixture):
    def test_gen_dry_run_does_not_create_output(self) -> None:
        self.apply()
        output = self.chromium / "out" / "LimitlessDebug"
        completed = self.tool("gen", "--dry-run")
        self.assertFalse(output.exists())
        self.assertIn("No output was modified", completed.stdout)
        self.assertIn("limitless_root", completed.stdout)

    def test_build_run_and_test_dry_runs_are_non_mutating(self) -> None:
        self.apply()
        self.generate()
        profiles = self.chromium / "out" / "LimitlessDebug" / "LimitlessProfiles"
        test_profiles = (
            self.chromium / "out" / "LimitlessDebug" / "LimitlessTestProfiles"
        )
        build = self.tool("build", "--dry-run")
        launch = self.tool("run", "--dry-run", "--fresh-profile")
        tests = self.tool("test", "--dry-run")
        self.assertIn("chrome browser_tests interactive_ui_tests", build.stdout)
        self.assertIn("--enable-features=Webium,SurfaceEmbed", launch.stdout)
        self.assertIn("LimitlessWebUIBrowserTest.*", tests.stdout)
        self.assertNotIn("--enable-features", tests.stdout)
        self.assertFalse(profiles.exists())
        self.assertFalse(test_profiles.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
