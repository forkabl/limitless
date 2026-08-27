#!/usr/bin/env python3
"""Contract tests for live Limitless CDP validation and shutdown ordering."""

from __future__ import annotations

import base64
import hashlib
import importlib.util
import json
import socket
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from typing import Any


sys.dont_write_bytecode = True
MODULE_PATH = Path(__file__).resolve().parents[1] / "e2e" / "cdp_close.py"
LAUNCH_SCRIPT = Path(__file__).resolve().parents[1] / "e2e" / "app_launch_smoke.sh"
SPEC = importlib.util.spec_from_file_location("limitless_cdp_close", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"could not load {MODULE_PATH}")
CDP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CDP)

EXPECTED_REVISION = "2b4613f0a24d234b139f1246028239a2f5dde58c"
EXPECTED_BUILD_ID = "limitless-phase1-macos-arm64-2b4613f0"
EXPECTED_URL = "chrome://webui-browser/"


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


class FakeCdpClient:
    def __init__(
        self,
        *,
        browser_revision: str = "@" + EXPECTED_REVISION,
        states: list[dict[str, Any]] | None = None,
    ) -> None:
        self.browser_revision = browser_revision
        self.states = list(states or [page_state()])
        self.commands: list[tuple[str, str, str | None]] = []

    def call(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        *,
        session_id: str | None = None,
        deadline: float,
    ) -> dict[str, Any]:
        del params, deadline
        self.commands.append(("call", method, session_id))
        if method == "Browser.getVersion":
            return {"revision": self.browser_revision}
        if method == "Target.getTargets":
            return {
                "targetInfos": [
                    {
                        "targetId": "limitless-shell-target",
                        "type": "page",
                        "url": EXPECTED_URL,
                    }
                ]
            }
        if method == "Target.attachToTarget":
            return {"sessionId": "limitless-shell-session"}
        if method == "Runtime.enable":
            return {}
        if method == "Runtime.evaluate":
            state = self.states.pop(0) if len(self.states) > 1 else self.states[0]
            return {"result": {"type": "object", "value": state}}
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
                        result = {
                            "targetInfos": [
                                {
                                    "targetId": "wire-target",
                                    "type": "page",
                                    "url": EXPECTED_URL,
                                }
                            ]
                        }
                    elif method == "Target.attachToTarget":
                        result = {"sessionId": "wire-session"}
                    elif method == "Runtime.enable":
                        result = {}
                    elif method == "Runtime.evaluate":
                        result = {"result": {"type": "object", "value": page_state()}}
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
    return CDP.validate_then_close(
        client,
        expected_url=EXPECTED_URL,
        expected_build_id=EXPECTED_BUILD_ID,
        expected_revision=EXPECTED_REVISION,
        timeout=1.0,
        poll_interval=0.0,
    )


class CdpSmokeContractTest(unittest.TestCase):
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
                timeout=2.0,
            )

        endpoint.join()
        self.assertEqual([], endpoint.errors)
        self.assertEqual("Browser.close", endpoint.commands[-1])
        self.assertEqual(EXPECTED_BUILD_ID, result["build_id"])

    def test_launch_script_wires_live_identity_validation_and_artifact(self) -> None:
        source = LAUNCH_SCRIPT.read_text(encoding="utf-8")

        for expected in (
            'python3 "$SCRIPT_DIR/cdp_close.py"',
            '--url "$SHELL_URL"',
            '--expected-build-id "$EXPECTED_BUILD_ID"',
            '--expected-chromium-revision "$CHROMIUM_REVISION"',
            '>"$CDP_RESULT"',
            'echo "cdp_validation=$CDP_RESULT"',
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
        self.assertEqual(2, methods.count("Runtime.evaluate"))
        self.assertLess(
            methods.index("Runtime.evaluate"), methods.index("Browser.close")
        )

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
