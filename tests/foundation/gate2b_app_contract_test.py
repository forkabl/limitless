#!/usr/bin/env python3
"""Contract tests for the real-app Gate 2B native boundary checkpoint."""

from __future__ import annotations

import binascii
import importlib.util
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib
from pathlib import Path
from typing import Any


sys.dont_write_bytecode = True
E2E_DIR = Path(__file__).resolve().parents[1] / "e2e"
MODULE_PATH = E2E_DIR / "gate2b_cdp.py"
LAUNCH_SCRIPT = E2E_DIR / "gate2b_app_smoke.sh"
NATIVE_PROBE = E2E_DIR / "macos_boundary_probe.swift"
SPEC = importlib.util.spec_from_file_location("limitless_gate2b_cdp", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"could not load {MODULE_PATH}")
GATE2B = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE2B)

EXPECTED_REVISION = "2b4613f0a24d234b139f1246028239a2f5dde58c"
BROWSER_PID = 4242


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    return (
        struct.pack("!I", len(data))
        + chunk_type
        + data
        + struct.pack("!I", binascii.crc32(chunk_type + data) & 0xFFFFFFFF)
    )


def png_bytes(width: int, height: int, *, alpha: int = 255) -> bytes:
    row = b"\x00" + bytes((0, 0, 0, alpha)) * width
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(
            b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 6, 0, 0, 0)
        )
        + png_chunk(b"IDAT", zlib.compress(row * height))
        + png_chunk(b"IEND", b"")
    )


PNG_BYTES = png_bytes(2, 1)


class FakeCdpClient:
    def __init__(
        self,
        *,
        targets: list[dict[str, Any]] | None = None,
        bounds: dict[str, Any] | None = None,
        revision: str = "@" + EXPECTED_REVISION,
        close_fails: bool = False,
    ) -> None:
        self.targets = targets or [
            {"targetId": "real-page", "type": "page", "url": "about:blank"}
        ]
        self.bounds = bounds or {
            "left": 10,
            "top": 20,
            "width": 2,
            "height": 1,
            "windowState": "normal",
        }
        self.revision = revision
        self.close_fails = close_fails
        self.commands: list[str] = []
        self.deadlines: list[float] = []

    def call(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        *,
        deadline: float,
    ) -> dict[str, Any]:
        self.deadlines.append(deadline)
        self.commands.append(method)
        if method == "Browser.getVersion":
            return {
                "product": "Chrome/154.0.8022.0",
                "protocolVersion": "1.3",
                "revision": self.revision,
            }
        if method == "Target.getTargets":
            return {"targetInfos": self.targets}
        if method == "Browser.getWindowForTarget":
            if (params or {}).get("targetId") != "real-page":
                raise AssertionError("window lookup did not use the real page target")
            return {"windowId": 7, "bounds": self.bounds}
        raise AssertionError(f"unexpected CDP command: {method}")

    def send_command(self, method: str) -> int:
        self.commands.append(method)
        if self.close_fails:
            raise RuntimeError("synthetic close failure")
        return len(self.commands)


def native_result(screenshot: Path, **overrides: Any) -> dict[str, Any]:
    result: dict[str, Any] = {
        "accessibility_permission": True,
        "ax_window_count": 1,
        "cg_window_id": 99,
        "display_scale": 1.0,
        "focus_identifier": f"{GATE2B.FOCUS_IDENTIFIER_PREFIX}42",
        "focus_method": "macos-accessibility-AXFocused",
        "focus_parent_reaches_window": True,
        "focus_succeeded": True,
        "matching_window_count": 1,
        "native_keyboard_input": False,
        "owner_pid": BROWSER_PID,
        "rail_identifier": GATE2B.RAIL_IDENTIFIER,
        "rail_parent_reaches_window": True,
        "rail_width": 216.0,
        "screen_capture_permission": True,
        "screenshot": str(screenshot),
        "screenshot_method": "screencapture-window-id",
        "window_bounds": {"x": 10, "y": 20, "width": 2, "height": 1},
        "window_title": "about:blank",
    }
    result.update(overrides)
    return result


def validate(
    client: FakeCdpClient,
    *,
    probe_overrides: dict[str, Any] | None = None,
    screenshot_bytes: bytes = PNG_BYTES,
    deadline: float | None = None,
    observed_probe_deadlines: list[float] | None = None,
) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="limitless-gate2b-contract-") as root:
        screenshot = Path(root) / "native.png"

        def probe(probe_deadline: float) -> dict[str, Any]:
            if observed_probe_deadlines is not None:
                observed_probe_deadlines.append(probe_deadline)
            screenshot.write_bytes(screenshot_bytes)
            return native_result(screenshot, **(probe_overrides or {}))

        return GATE2B.validate_then_close(
            client,
            browser_pid=BROWSER_PID,
            expected_revision=EXPECTED_REVISION,
            screenshot_path=screenshot,
            native_probe=probe,
            deadline=deadline if deadline is not None else time.monotonic() + 1,
        )


class Gate2BAppContractTest(unittest.TestCase):
    def test_real_page_native_boundary_capture_and_focus_precede_close(self) -> None:
        client = FakeCdpClient()

        result = validate(client)

        self.assertEqual("Browser.close", client.commands[-1])
        self.assertLess(
            client.commands.index("Browser.getWindowForTarget"),
            client.commands.index("Browser.close"),
        )
        self.assertEqual(1, result["page_target_count"])
        self.assertEqual(0, result["webui_browser_target_count"])
        self.assertEqual("Browser.close", result["browser_close_method"])
        self.assertTrue(result["browser_close_requested"])
        native = result["native_boundary"]
        self.assertEqual("screencapture-window-id", native["screenshot_method"])
        self.assertEqual("macos-accessibility-AXFocused", native["focus_method"])
        self.assertFalse(native["native_keyboard_input"])
        self.assertEqual(2, native["native_window_screenshot_width"])
        self.assertEqual(1, native["native_window_screenshot_height"])

    def test_native_screenshot_validation_accepts_1x_and_2x_backing_scales(
        self,
    ) -> None:
        bounds = {
            "left": 10,
            "top": 20,
            "width": 120,
            "height": 80,
            "windowState": "normal",
        }
        native_bounds = {"x": 10, "y": 20, "width": 120, "height": 80}
        for scale in (1.0, 2.0):
            with self.subTest(scale=scale):
                client = FakeCdpClient(bounds=bounds)
                result = validate(
                    client,
                    probe_overrides={
                        "display_scale": scale,
                        "window_bounds": native_bounds,
                    },
                    screenshot_bytes=png_bytes(
                        round(native_bounds["width"] * scale),
                        round(native_bounds["height"] * scale),
                    ),
                )

                native = result["native_boundary"]
                self.assertEqual(
                    round(120 * scale), native["native_window_screenshot_width"]
                )
                self.assertEqual(
                    round(80 * scale), native["native_window_screenshot_height"]
                )
                self.assertEqual("Browser.close", client.commands[-1])

    def test_native_screenshot_validation_rejects_wrong_backing_scale(self) -> None:
        client = FakeCdpClient(
            bounds={
                "left": 10,
                "top": 20,
                "width": 120,
                "height": 80,
                "windowState": "normal",
            }
        )
        with self.assertRaisesRegex(
            RuntimeError, "dimensions do not match the whole native window"
        ):
            validate(
                client,
                probe_overrides={
                    "display_scale": 1.0,
                    "window_bounds": {
                        "x": 10,
                        "y": 20,
                        "width": 120,
                        "height": 80,
                    },
                },
                screenshot_bytes=png_bytes(240, 160),
            )
        self.assertEqual("Browser.close", client.commands[-1])

    def test_native_screenshot_validation_rejects_transparent_capture(self) -> None:
        client = FakeCdpClient()
        with self.assertRaisesRegex(RuntimeError, "fully transparent"):
            validate(client, screenshot_bytes=png_bytes(2, 1, alpha=0))
        self.assertEqual("Browser.close", client.commands[-1])

    def test_fractional_native_bounds_allow_only_scale_quantization(self) -> None:
        bounds = {
            "left": 10,
            "top": 20,
            "width": 120.25,
            "height": 80.25,
            "windowState": "normal",
        }
        native_bounds = {"x": 10, "y": 20, "width": 120.25, "height": 80.25}
        client = FakeCdpClient(bounds=bounds)
        result = validate(
            client,
            probe_overrides={
                "display_scale": 2.0,
                "window_bounds": native_bounds,
            },
            screenshot_bytes=png_bytes(241, 160),
        )
        native = result["native_boundary"]
        self.assertEqual(241, native["native_window_screenshot_width"])
        self.assertEqual(160, native["native_window_screenshot_height"])
        self.assertLessEqual(
            native["native_window_screenshot_aspect_min"], 241 / 160
        )
        self.assertGreaterEqual(
            native["native_window_screenshot_aspect_max"], 241 / 160
        )

        for width, height in ((242, 160), (160, 241)):
            with self.subTest(width=width, height=height):
                client = FakeCdpClient(bounds=bounds)
                with self.assertRaisesRegex(
                    RuntimeError, "dimensions do not match the whole native window"
                ):
                    validate(
                        client,
                        probe_overrides={
                            "display_scale": 2.0,
                            "window_bounds": native_bounds,
                        },
                        screenshot_bytes=png_bytes(width, height),
                    )
                self.assertEqual("Browser.close", client.commands[-1])

    def test_native_screenshot_validation_rejects_unsupported_scale(self) -> None:
        client = FakeCdpClient()
        with self.assertRaisesRegex(RuntimeError, "unsupported display scale"):
            validate(client, probe_overrides={"display_scale": 1.5})
        self.assertEqual("Browser.close", client.commands[-1])

    def test_post_capture_cdp_window_change_fails_closed(self) -> None:
        class MovingWindowClient(FakeCdpClient):
            def __init__(self) -> None:
                super().__init__()
                self.window_lookups = 0

            def call(
                self,
                method: str,
                params: dict[str, Any] | None = None,
                *,
                deadline: float,
            ) -> dict[str, Any]:
                result = super().call(method, params, deadline=deadline)
                if method == "Browser.getWindowForTarget":
                    self.window_lookups += 1
                    if self.window_lookups == 2:
                        return {
                            "windowId": result["windowId"],
                            "bounds": {**result["bounds"], "left": 40},
                        }
                return result

        client = MovingWindowClient()
        with self.assertRaisesRegex(RuntimeError, "left changed during capture"):
            validate(client)
        self.assertEqual(2, client.window_lookups)
        self.assertEqual("Browser.close", client.commands[-1])

    def test_webui_browser_target_is_rejected_and_browser_still_closes(self) -> None:
        client = FakeCdpClient(
            targets=[
                {"targetId": "shell", "type": "page", "url": "chrome://webui-browser/"},
                {"targetId": "real-page", "type": "page", "url": "about:blank"},
            ]
        )

        with self.assertRaisesRegex(RuntimeError, "WebUI Browser shell target"):
            validate(client)

        self.assertEqual("Browser.close", client.commands[-1])

    def test_second_real_page_is_rejected_and_browser_still_closes(self) -> None:
        client = FakeCdpClient(
            targets=[
                {"targetId": "real-page", "type": "page", "url": "about:blank"},
                {"targetId": "other", "type": "page", "url": "about:blank"},
            ]
        )

        with self.assertRaisesRegex(RuntimeError, "exactly one real page target"):
            validate(client)

        self.assertEqual("Browser.close", client.commands[-1])

    def test_pid_ax_parent_or_focus_failure_still_closes(self) -> None:
        failure_cases = (
            ({"owner_pid": BROWSER_PID + 1}, "owner PID mismatch"),
            ({"rail_parent_reaches_window": False}, "parent chain"),
            ({"focus_succeeded": False}, "boundary focus"),
            ({"native_keyboard_input": True}, "native keyboard input"),
        )
        for overrides, message in failure_cases:
            with self.subTest(overrides=overrides):
                client = FakeCdpClient()
                with self.assertRaisesRegex(RuntimeError, message):
                    validate(client, probe_overrides=overrides)
                self.assertEqual("Browser.close", client.commands[-1])

    def test_bounds_or_corrupt_native_png_failure_still_closes(self) -> None:
        client = FakeCdpClient()
        with self.assertRaisesRegex(RuntimeError, "bounds mismatch"):
            validate(
                client,
                probe_overrides={
                    "window_bounds": {"x": 11, "y": 20, "width": 20, "height": 1}
                },
            )
        self.assertEqual("Browser.close", client.commands[-1])

        client = FakeCdpClient()
        with self.assertRaisesRegex(RuntimeError, "native window screenshot"):
            validate(client, screenshot_bytes=b"not-a-png")
        self.assertEqual("Browser.close", client.commands[-1])

    def test_close_failure_is_reported_after_successful_validation(self) -> None:
        client = FakeCdpClient(close_fails=True)

        with self.assertRaisesRegex(RuntimeError, "could not request Browser.close"):
            validate(client)

        self.assertEqual("Browser.close", client.commands[-1])

    def test_validation_and_close_failures_are_both_reported(self) -> None:
        client = FakeCdpClient(
            targets=[
                {"targetId": "shell", "type": "page", "url": "chrome://webui-browser/"}
            ],
            close_fails=True,
        )

        with self.assertRaisesRegex(
            RuntimeError,
            "validation failed: .*WebUI Browser shell target.*; "
            "Browser.close request also failed: synthetic close failure",
        ):
            validate(client)

        self.assertEqual("Browser.close", client.commands[-1])

    def test_interrupt_after_cdp_connection_still_requests_close(self) -> None:
        class InterruptingClient(FakeCdpClient):
            def call(
                self,
                method: str,
                params: dict[str, Any] | None = None,
                *,
                deadline: float,
            ) -> dict[str, Any]:
                del params, deadline
                self.commands.append(method)
                raise KeyboardInterrupt

        client = InterruptingClient()

        with self.assertRaises(KeyboardInterrupt):
            validate(client)

        self.assertEqual(["Browser.getVersion", "Browser.close"], client.commands)

    def test_one_deadline_covers_cdp_and_the_native_probe(self) -> None:
        client = FakeCdpClient()
        deadline = time.monotonic() + 1
        observed_probe_deadlines: list[float] = []

        validate(
            client,
            deadline=deadline,
            observed_probe_deadlines=observed_probe_deadlines,
        )

        self.assertTrue(client.deadlines)
        self.assertTrue(all(value == deadline for value in client.deadlines))
        self.assertEqual([deadline], observed_probe_deadlines)

    def test_expired_deadline_prevents_native_probe_but_still_closes(self) -> None:
        client = FakeCdpClient()
        observed_probe_deadlines: list[float] = []

        with self.assertRaisesRegex(
            TimeoutError, "before starting the native boundary probe"
        ):
            validate(
                client,
                deadline=time.monotonic() - 1,
                observed_probe_deadlines=observed_probe_deadlines,
            )

        self.assertEqual([], observed_probe_deadlines)
        self.assertEqual("Browser.close", client.commands[-1])

    def test_native_probe_reserves_time_for_child_cleanup(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="limitless-gate2b-probe-timeout-"
        ) as root:
            probe = Path(root) / "probe"
            probe.write_text("unused", encoding="utf-8")

            with self.assertRaisesRegex(
                TimeoutError, "native boundary probe with bounded cleanup"
            ):
                GATE2B.run_native_probe(
                    probe,
                    browser_pid=BROWSER_PID,
                    screenshot_path=Path(root) / "native.png",
                    deadline=time.monotonic() + GATE2B.NATIVE_PROBE_CLEANUP_RESERVE / 2,
                )

    def test_reaper_records_and_consumes_an_already_exited_child(self) -> None:
        launch = LAUNCH_SCRIPT.read_text(encoding="utf-8")
        function_start = launch.index("reap_browser() {")
        function_end = launch.index("\n}\n\ncleanup()", function_start) + 2
        reap_function = launch[function_start:function_end]
        with tempfile.TemporaryDirectory(prefix="limitless-gate2b-reaper-") as root:
            status_file = Path(root) / "browser_exit_status.txt"
            script = f"""
set -euo pipefail
{reap_function}
BROWSER_STATUS_FILE="$1"
BROWSER_REAPED=0
BROWSER_STATUS=""
bash -c 'exit 7' &
LAUNCHED_BROWSER_PID=$!
sleep 0.1
reap_browser
AFTER_WAIT_STATE="$(ps -p "$LAUNCHED_BROWSER_PID" -o stat= 2>/dev/null || true)"
printf 'reaped=%s\nstatus=%s\nafter_wait_state=%s\n' \
  "$BROWSER_REAPED" "$BROWSER_STATUS" "$AFTER_WAIT_STATE"
"""
            completed = subprocess.run(
                ["bash", "-c", script, "gate2b-reaper-contract", str(status_file)],
                check=True,
                capture_output=True,
                text=True,
            )

            self.assertEqual(
                "reaped=1\nstatus=7\nafter_wait_state=\n", completed.stdout
            )
            status = status_file.read_text(encoding="utf-8")
            self.assertIn("browser_exit_status=7\n", status)
            self.assertRegex(status, r"browser_pid=[1-9][0-9]*\n")

    def test_launch_and_native_probe_keep_the_outside_in_contract_explicit(
        self,
    ) -> None:
        launch = LAUNCH_SCRIPT.read_text(encoding="utf-8")
        probe = NATIVE_PROBE.read_text(encoding="utf-8")

        for required in (
            "--disable-features=Webium",
            "--limitless-swiftui-boundary-probe",
            "--remote-debugging-port=0",
            "--assert-absent-window-id",
            "-warnings-as-errors",
            "Print :SCMRevision",
            '[[ "$BUNDLE_SCM_REVISION" == "$CHROMIUM_REVISION"-* ]]',
            "shutdown=Browser.close",
            "native_keyboard_input=false",
        ):
            self.assertIn(required, launch)
        self.assertNotIn("--enable-features=Webium", launch)
        self.assertNotIn("Page.captureScreenshot", launch)
        for required in (
            "E2E_DEADLINE_SECONDS",
            "VALIDATION_TIMEOUT",
            '--timeout "$VALIDATION_TIMEOUT"',
            'wait "$LAUNCHED_BROWSER_PID"',
            "browser_exit_status=$BROWSER_STATUS",
            "browser_exit_status_file=$BROWSER_STATUS_FILE",
        ):
            self.assertIn(required, launch)
        self.assertNotIn('--timeout "$STARTUP_TIMEOUT"', launch)
        cleanup = launch.split("cleanup() {", 1)[1].split("\n}", 1)[0]
        self.assertIn("reap_browser", cleanup)
        for required in (
            "CGPreflightScreenCaptureAccess()",
            "AXIsProcessTrusted()",
            "kCGWindowOwnerPID",
            ".optionAll",
            "kAXIdentifierAttribute",
            "kAXParentAttribute",
            "kAXFocusedAttribute",
            '"/usr/sbin/screencapture"',
            '"-l\\(window.identifier)"',
            '"-a"',
            '"-t", "png"',
            "CGDisplayCopyDisplayMode",
            "pixelWidth",
            "process.terminate()",
            "Darwin.kill(process.processIdentifier, SIGKILL)",
        ):
            self.assertIn(required, probe)
        self.assertNotIn("CGWindowListCreateImage", probe)
        self.assertNotIn("CGEventPost", probe)
        raw_lookup = probe.split("private func rawWindowRecordExists", 1)[1].split(
            "private func copyAttribute", 1
        )[0]
        self.assertIn(
            "CGWindowListCopyWindowInfo(.optionAll, kCGNullWindowID)", raw_lookup
        )
        self.assertIn("kCGWindowOwnerPID", raw_lookup)
        self.assertIn("kCGWindowNumber", raw_lookup)
        self.assertNotIn("kCGWindowLayer", raw_lookup)
        self.assertNotIn("kCGWindowBounds", raw_lookup)


if __name__ == "__main__":
    unittest.main(verbosity=2)
