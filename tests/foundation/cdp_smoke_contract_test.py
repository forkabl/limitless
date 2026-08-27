#!/usr/bin/env python3
"""Contract tests for live Limitless CDP validation and shutdown ordering."""

from __future__ import annotations

import base64
import binascii
import hashlib
import importlib.util
import json
import socket
import struct
import sys
import tempfile
import threading
import unittest
import zlib
from pathlib import Path
from typing import Any


sys.dont_write_bytecode = True
MODULE_PATH = Path(__file__).resolve().parents[1] / "e2e" / "cdp_close.py"
LAUNCH_SCRIPT = Path(__file__).resolve().parents[1] / "e2e" / "app_launch_smoke.sh"
CHROMIUM_SRC = Path(__file__).resolve().parents[3] / "chromium" / "src"
SPEC = importlib.util.spec_from_file_location("limitless_cdp_close", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"could not load {MODULE_PATH}")
CDP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CDP)

EXPECTED_REVISION = "2b4613f0a24d234b139f1246028239a2f5dde58c"
EXPECTED_BUILD_ID = "limitless-phase1-macos-arm64-2b4613f0"
EXPECTED_URL = "chrome://webui-browser/"
NAVIGATION_URL = "http://127.0.0.1:43210/limitless-gate2-navigation.html"


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    return (
        struct.pack("!I", len(data))
        + chunk_type
        + data
        + struct.pack("!I", binascii.crc32(chunk_type + data) & 0xFFFFFFFF)
    )


PNG_BYTES = (
    b"\x89PNG\r\n\x1a\n"
    + png_chunk(b"IHDR", struct.pack("!IIBBBBB", 2, 1, 8, 6, 0, 0, 0))
    + png_chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00\xff\x00\xff\x00\xff"))
    + png_chunk(b"IEND", b"")
)


def page_state(
    *,
    ready: bool = True,
    build_id: str = EXPECTED_BUILD_ID,
    revision: str = EXPECTED_REVISION,
) -> dict[str, Any]:
    return {
        "url": EXPECTED_URL,
        "ready": ready,
        "shellPresent": True,
        "markerPresent": True,
        "buildId": build_id,
        "chromiumRevision": revision,
    }


def address_state(*, focused: bool = False, value: str = "") -> dict[str, Any]:
    return {
        "addressPresent": True,
        "inputPresent": True,
        "clickable": True,
        "centerX": 320.0,
        "centerY": 48.0,
        "appHit": "webui-browser-searchbox#address",
        "addressHit": "cr-searchbox-input#input",
        "searchboxHit": "input#input",
        "focused": focused,
        "value": value,
    }


def navigation_state() -> dict[str, Any]:
    return {
        "url": NAVIGATION_URL,
        "ready": True,
        "markerPresent": True,
        "checkpoint": CDP.NAVIGATION_CHECKPOINT,
        "title": CDP.NAVIGATION_TITLE,
    }


class FakeCdpClient:
    def __init__(
        self,
        *,
        browser_revision: str = "@" + EXPECTED_REVISION,
        states: list[dict[str, Any]] | None = None,
        navigated_state: dict[str, Any] | None = None,
        replace_target_after_submit: bool = False,
        screenshot_bytes: bytes = PNG_BYTES,
    ) -> None:
        self.browser_revision = browser_revision
        self.states = list(states or [page_state()])
        self.navigated_state = navigated_state or navigation_state()
        self.replace_target_after_submit = replace_target_after_submit
        self.screenshot_bytes = screenshot_bytes
        self.commands: list[tuple[str, str, str | None]] = []
        self.focused = False
        self.inserted_text = ""
        self.submitted = False
        self.shell_state_evaluations = 0

    def call(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        *,
        session_id: str | None = None,
        deadline: float,
    ) -> dict[str, Any]:
        del deadline
        self.commands.append(("call", method, session_id))
        if method == "Browser.getVersion":
            return {"revision": self.browser_revision}
        if method == "Target.getTargets":
            targets = [
                {
                    "targetId": "limitless-shell-target",
                    "type": "page",
                    "url": EXPECTED_URL,
                },
                {
                    "targetId": "replacement-tab-target"
                    if self.submitted and self.replace_target_after_submit
                    else "real-tab-target",
                    "type": "page",
                    "url": NAVIGATION_URL if self.submitted else "about:blank",
                },
            ]
            return {"targetInfos": targets}
        if method == "Target.attachToTarget":
            target_id = (params or {}).get("targetId")
            if target_id == "limitless-shell-target":
                return {"sessionId": "limitless-shell-session"}
            if target_id == "real-tab-target":
                return {"sessionId": "navigated-tab-session"}
            raise AssertionError(f"unexpected target attachment: {target_id}")
        if method in ("Runtime.enable", "Page.enable"):
            return {}
        if method == "Runtime.evaluate":
            expression = (params or {}).get("expression")
            if expression == CDP.SHELL_STATE_EXPRESSION:
                self.shell_state_evaluations += 1
                state = self.states.pop(0) if len(self.states) > 1 else self.states[0]
            elif expression == CDP.ADDRESS_STATE_EXPRESSION:
                state = address_state(focused=self.focused, value=self.inserted_text)
            elif expression == CDP.NAVIGATION_STATE_EXPRESSION:
                state = self.navigated_state
            else:
                raise AssertionError(f"unexpected Runtime.evaluate: {expression}")
            return {"result": {"type": "object", "value": state}}
        if method == "Input.dispatchMouseEvent":
            event = params or {}
            if event.get("type") == "mouseReleased":
                self.focused = True
            return {}
        if method == "Input.dispatchKeyEvent":
            event = params or {}
            if event.get("type") == "rawKeyDown" and event.get("key") == "Enter":
                if self.inserted_text != NAVIGATION_URL:
                    raise AssertionError("Enter submitted an unexpected address")
                self.submitted = True
            return {}
        if method == "Input.insertText":
            if not self.focused:
                raise AssertionError("text inserted before the address was focused")
            self.inserted_text = str((params or {}).get("text", ""))
            return {}
        if method == "Page.captureScreenshot":
            return {"data": base64.b64encode(self.screenshot_bytes).decode("ascii")}
        raise AssertionError(f"unexpected CDP call: {method}")

    def send_command(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        *,
        session_id: str | None = None,
    ) -> int:
        del params
        self.commands.append(("send", method, session_id))
        return len(self.commands)


class FakeBrowserEndpoint:
    def __init__(self) -> None:
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.commands: list[str] = []
        self.errors: list[BaseException] = []
        self.focused = False
        self.inserted_text = ""
        self.submitted = False
        self.thread = threading.Thread(target=self._serve, daemon=True)

    @staticmethod
    def _read_exact(connection: socket.socket, length: int) -> bytes:
        result = bytearray()
        while len(result) < length:
            block = connection.recv(length - len(result))
            if not block:
                raise RuntimeError("fake CDP client disconnected")
            result.extend(block)
        return bytes(result)

    @classmethod
    def _read_client_message(cls, connection: socket.socket) -> dict[str, Any]:
        first, second = cls._read_exact(connection, 2)
        if first != 0x81 or not second & 0x80:
            raise RuntimeError("fake CDP server received an invalid client frame")
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", cls._read_exact(connection, 2))[0]
        elif length == 127:
            length = struct.unpack("!Q", cls._read_exact(connection, 8))[0]
        mask = cls._read_exact(connection, 4)
        payload = cls._read_exact(connection, length)
        unmasked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        message = json.loads(unmasked.decode("utf-8"))
        if not isinstance(message, dict):
            raise RuntimeError("fake CDP server received a non-object message")
        return message

    @staticmethod
    def _send_result(
        connection: socket.socket, command_id: int, result: dict[str, Any]
    ) -> None:
        payload = json.dumps(
            {"id": command_id, "result": result}, separators=(",", ":")
        ).encode("utf-8")
        if len(payload) < 126:
            header = bytes((0x81, len(payload)))
        elif len(payload) <= 0xFFFF:
            header = bytes((0x81, 126)) + struct.pack("!H", len(payload))
        else:
            header = bytes((0x81, 127)) + struct.pack("!Q", len(payload))
        connection.sendall(header + payload)

    @staticmethod
    def _handshake(connection: socket.socket) -> None:
        request = bytearray()
        while b"\r\n\r\n" not in request:
            request.extend(connection.recv(4096))
        headers: dict[str, str] = {}
        for line in request.decode("latin-1").split("\r\n")[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
        key = headers["sec-websocket-key"]
        accept = base64.b64encode(
            hashlib.sha1((key + CDP.WEBSOCKET_GUID).encode("ascii")).digest()
        ).decode("ascii")
        response = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
        ).encode("ascii")
        connection.sendall(response)

    def _serve(self) -> None:
        try:
            self.listener.settimeout(3)
            connection, _ = self.listener.accept()
            with connection:
                connection.settimeout(3)
                self._handshake(connection)
                while True:
                    message = self._read_client_message(connection)
                    method = message["method"]
                    self.commands.append(method)
                    if method == "Browser.close":
                        return
                    if method == "Browser.getVersion":
                        result = {"revision": EXPECTED_REVISION}
                    elif method == "Target.getTargets":
                        targets = [
                            {
                                "targetId": "wire-shell-target",
                                "type": "page",
                                "url": EXPECTED_URL,
                            },
                            {
                                "targetId": "wire-real-tab-target",
                                "type": "page",
                                "url": self.inserted_text
                                if self.submitted
                                else "about:blank",
                            },
                        ]
                        result = {"targetInfos": targets}
                    elif method == "Target.attachToTarget":
                        target_id = message.get("params", {}).get("targetId")
                        if target_id == "wire-shell-target":
                            result = {"sessionId": "wire-shell-session"}
                        elif target_id == "wire-real-tab-target":
                            result = {"sessionId": "wire-navigation-session"}
                        else:
                            raise RuntimeError(
                                f"unexpected wire target attachment: {target_id}"
                            )
                    elif method in ("Runtime.enable", "Page.enable"):
                        result = {}
                    elif method == "Runtime.evaluate":
                        expression = message.get("params", {}).get("expression")
                        if expression == CDP.SHELL_STATE_EXPRESSION:
                            state = page_state()
                        elif expression == CDP.ADDRESS_STATE_EXPRESSION:
                            state = address_state(
                                focused=self.focused, value=self.inserted_text
                            )
                        elif expression == CDP.NAVIGATION_STATE_EXPRESSION:
                            state = {
                                **navigation_state(),
                                "url": self.inserted_text,
                            }
                        else:
                            raise RuntimeError(
                                f"unexpected wire Runtime.evaluate: {expression}"
                            )
                        result = {"result": {"type": "object", "value": state}}
                    elif method == "Input.dispatchMouseEvent":
                        event = message.get("params", {})
                        if event.get("type") == "mouseReleased":
                            self.focused = True
                        result = {}
                    elif method == "Input.dispatchKeyEvent":
                        event = message.get("params", {})
                        if (
                            event.get("type") == "rawKeyDown"
                            and event.get("key") == "Enter"
                        ):
                            self.submitted = True
                        result = {}
                    elif method == "Input.insertText":
                        self.inserted_text = str(
                            message.get("params", {}).get("text", "")
                        )
                        result = {}
                    elif method == "Page.captureScreenshot":
                        result = {"data": base64.b64encode(PNG_BYTES).decode("ascii")}
                    else:
                        raise RuntimeError(f"unexpected wire CDP method: {method}")
                    self._send_result(connection, message["id"], result)
        except BaseException as exc:
            self.errors.append(exc)
        finally:
            self.listener.close()

    def start(self) -> None:
        self.thread.start()

    def join(self) -> None:
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            raise RuntimeError("fake CDP server did not stop")


def validate(client: FakeCdpClient) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="limitless-cdp-screenshot-") as root:
        return CDP.validate_then_close(
            client,
            expected_url=EXPECTED_URL,
            expected_build_id=EXPECTED_BUILD_ID,
            expected_revision=EXPECTED_REVISION,
            navigation_url=NAVIGATION_URL,
            screenshot_path=Path(root) / "shell.png",
            timeout=1.0,
            poll_interval=0.0,
        )


class CdpSmokeContractTest(unittest.TestCase):
    def test_pinned_chromium_preserves_cmd_l_focus_command_path(self) -> None:
        accelerator_source = (
            CHROMIUM_SRC / "chrome/browser/ui/cocoa/accelerators_cocoa.mm"
        ).read_text(encoding="utf-8")
        controller_source = (
            CHROMIUM_SRC / "chrome/browser/ui/browser_command_controller.cc"
        ).read_text(encoding="utf-8")
        command_source = (
            CHROMIUM_SRC / "chrome/browser/ui/browser_commands.cc"
        ).read_text(encoding="utf-8")
        webui_window_source = (
            CHROMIUM_SRC / "chrome/browser/ui/webui_browser/webui_browser_window.cc"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "{IDC_FOCUS_LOCATION, ui::EF_COMMAND_DOWN, ui::VKEY_L}",
            accelerator_source,
        )
        self.assertIn("case IDC_FOCUS_LOCATION:", controller_source)
        self.assertIn(
            "BrowserWindow::FromBrowser(browser)->SetFocusToLocationBar(true);",
            command_source,
        )
        self.assertIn(
            "page->SetFocusToLocationBar(is_user_initiated);", webui_window_source
        )

    def test_stdlib_websocket_runs_the_full_validation_sequence(self) -> None:
        endpoint = FakeBrowserEndpoint()
        endpoint.start()
        with tempfile.TemporaryDirectory(prefix="limitless-cdp-contract-") as root:
            profile = Path(root)
            (profile / "DevToolsActivePort").write_text(
                f"{endpoint.port}\n/devtools/browser/foundation-test\n",
                encoding="utf-8",
            )

            result = CDP.validate_profile(
                profile,
                expected_url=EXPECTED_URL,
                expected_build_id=EXPECTED_BUILD_ID,
                expected_revision=EXPECTED_REVISION,
                screenshot_path=Path(root) / "shell.png",
                timeout=2.0,
            )
            screenshot = (Path(root) / "shell.png").read_bytes()

        endpoint.join()
        self.assertEqual([], endpoint.errors)
        self.assertEqual("Browser.close", endpoint.commands[-1])
        self.assertEqual(EXPECTED_BUILD_ID, result["build_id"])
        self.assertEqual(CDP.NAVIGATION_CHECKPOINT, result["navigation_checkpoint"])
        self.assertEqual(PNG_BYTES, screenshot)

    def test_launch_script_wires_live_identity_validation_and_artifact(self) -> None:
        source = LAUNCH_SCRIPT.read_text(encoding="utf-8")

        for expected in (
            'python3 "$SCRIPT_DIR/cdp_close.py"',
            '--url "$SHELL_URL"',
            '--expected-build-id "$EXPECTED_BUILD_ID"',
            '--expected-chromium-revision "$CHROMIUM_REVISION"',
            '--screenshot "$SHELL_RENDERER_SCREENSHOT"',
            '>"$CDP_RESULT"',
            'echo "cdp_validation=$CDP_RESULT"',
            'echo "navigation_checkpoint=address_control_local_page"',
            'echo "shell_renderer_screenshot=$SHELL_RENDERER_SCREENSHOT"',
        ):
            self.assertIn(expected, source)

    def test_readiness_and_identity_are_verified_before_close(self) -> None:
        client = FakeCdpClient(states=[page_state(ready=False), page_state()])

        result = validate(client)

        self.assertTrue(result["ready"])
        self.assertEqual(EXPECTED_BUILD_ID, result["build_id"])
        self.assertEqual(EXPECTED_REVISION, result["chromium_revision"])
        self.assertEqual("exact", result["browser_revision_status"])
        methods = [method for _, method, _ in client.commands]
        self.assertEqual("Browser.close", methods[-1])
        self.assertEqual(2, client.shell_state_evaluations)
        self.assertLess(
            methods.index("Runtime.evaluate"), methods.index("Browser.close")
        )

    def test_live_address_navigation_and_screenshot_precede_close(self) -> None:
        client = FakeCdpClient()

        result = validate(client)

        self.assertEqual(
            "cdp-coordinate-click-insert-text-enter", result["address_input_method"]
        )
        self.assertEqual(NAVIGATION_URL, result["navigation_url"])
        self.assertEqual(CDP.NAVIGATION_CHECKPOINT, result["navigation_checkpoint"])
        self.assertEqual("real-tab-target", result["pre_navigation_target_id"])
        self.assertEqual(
            result["pre_navigation_target_id"], result["navigation_target_id"]
        )
        self.assertEqual(1, result["real_tab_count"])
        self.assertEqual(
            {
                "address": "cr-searchbox-input#input",
                "app": "webui-browser-searchbox#address",
                "input": "input#input",
            },
            result["address_hit_test"],
        )
        self.assertEqual(len(PNG_BYTES), result["shell_renderer_screenshot_bytes"])
        self.assertEqual(2, result["shell_renderer_screenshot_width"])
        self.assertEqual(1, result["shell_renderer_screenshot_height"])
        methods = [method for _, method, _ in client.commands]
        self.assertLess(
            methods.index("Input.dispatchMouseEvent"),
            methods.index("Input.insertText"),
        )
        capture_indexes = [
            index
            for index, method in enumerate(methods)
            if method == "Page.captureScreenshot"
        ]
        self.assertEqual(2, len(capture_indexes))
        self.assertLess(methods.index("Input.insertText"), capture_indexes[-1])
        self.assertLess(
            methods.index("Page.captureScreenshot"), methods.index("Browser.close")
        )

    def test_address_navigation_rejects_a_replacement_tab_target(self) -> None:
        client = FakeCdpClient(replace_target_after_submit=True)

        with self.assertRaisesRegex(
            RuntimeError, "address submission replaced or opened a different real tab"
        ):
            validate(client)

        self.assertEqual("Browser.close", client.commands[-1][1])

    def test_truncated_png_is_rejected_and_browser_still_closes(self) -> None:
        client = FakeCdpClient(screenshot_bytes=CDP.PNG_SIGNATURE + b"not-a-png")

        with self.assertRaisesRegex(RuntimeError, "truncated PNG chunk"):
            validate(client)

        self.assertEqual("Browser.close", client.commands[-1][1])

    def test_navigation_checkpoint_mismatch_still_requests_clean_close(self) -> None:
        bad_state = {**navigation_state(), "checkpoint": "wrong-checkpoint"}
        client = FakeCdpClient(navigated_state=bad_state)

        with self.assertRaisesRegex(RuntimeError, "navigation checkpoint mismatch"):
            validate(client)

        self.assertEqual("Browser.close", client.commands[-1][1])

    def test_build_id_mismatch_fails_and_still_requests_clean_close(self) -> None:
        client = FakeCdpClient(states=[page_state(build_id="wrong-build")])

        with self.assertRaisesRegex(RuntimeError, "build ID mismatch"):
            validate(client)

        self.assertEqual("Browser.close", client.commands[-1][1])

    def test_marker_revision_mismatch_fails(self) -> None:
        client = FakeCdpClient(states=[page_state(revision="0" * 40)])

        with self.assertRaisesRegex(RuntimeError, "marker Chromium revision mismatch"):
            validate(client)

        self.assertEqual("Browser.close", client.commands[-1][1])

    def test_live_browser_revision_mismatch_fails_before_page_evaluation(self) -> None:
        client = FakeCdpClient(browser_revision="1" * 40)

        with self.assertRaisesRegex(RuntimeError, "live browser revision mismatch"):
            validate(client)

        methods = [method for _, method, _ in client.commands]
        self.assertNotIn("Runtime.evaluate", methods)
        self.assertEqual("Browser.close", methods[-1])

    def test_unversioned_local_browser_revision_sentinel_is_recorded(self) -> None:
        client = FakeCdpClient(browser_revision="@" + "0" * 40)

        result = validate(client)

        self.assertEqual("@" + "0" * 40, result["browser_revision"])
        self.assertEqual("unversioned-local", result["browser_revision_status"])
        self.assertEqual(EXPECTED_REVISION, result["chromium_revision"])
        self.assertEqual("Browser.close", client.commands[-1][1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
