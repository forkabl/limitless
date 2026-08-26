#!/usr/bin/env python3
"""Validate the live Limitless shell through CDP, then close the browser."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import struct
import sys
import time
from pathlib import Path
from typing import Any


WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_HANDSHAKE_BYTES = 65536
MAX_MESSAGE_BYTES = 16 * 1024 * 1024
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
UNVERSIONED_LOCAL_REVISION = "0" * 40
DEFAULT_SHELL_URL = "chrome://webui-browser/"

SHELL_STATE_EXPRESSION = r"""
(() => {
  const shell = document.querySelector('#limitless-shell');
  const marker = document.querySelector('#limitless-build-marker');
  return {
    url: location.href,
    ready: shell instanceof HTMLElement && shell.dataset.ready === 'true',
    shellPresent: shell instanceof HTMLElement,
    markerPresent: marker instanceof HTMLElement,
    buildId: marker instanceof HTMLElement ? marker.dataset.buildId ?? '' : '',
    chromiumRevision: marker instanceof HTMLElement
        ? marker.dataset.chromiumRevision ?? ''
        : '',
  };
})()
""".strip()


def read_debugger_endpoint(profile: Path) -> tuple[int, str]:
    endpoint_file = profile / "DevToolsActivePort"
    try:
        lines = endpoint_file.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise RuntimeError(f"could not read {endpoint_file}: {exc}") from exc
    if len(lines) < 2:
        raise RuntimeError("DevToolsActivePort did not contain a browser endpoint")
    try:
        port = int(lines[0])
    except ValueError as exc:
        raise RuntimeError("DevToolsActivePort contained an invalid port") from exc
    websocket_path = lines[1]
    if not 1 <= port <= 65535:
        raise RuntimeError(f"DevToolsActivePort contained an invalid port: {port}")
    if not websocket_path.startswith("/") or "\r" in websocket_path:
        raise RuntimeError("DevToolsActivePort contained an invalid WebSocket path")
    return port, websocket_path


def _read_handshake(connection: socket.socket) -> tuple[bytes, bytearray]:
    response = bytearray()
    while b"\r\n\r\n" not in response:
        block = connection.recv(4096)
        if not block:
            raise RuntimeError("WebSocket handshake ended before HTTP headers arrived")
        response.extend(block)
        if len(response) > MAX_HANDSHAKE_BYTES:
            raise RuntimeError("WebSocket handshake response was too large")
    header_end = response.index(b"\r\n\r\n") + 4
    return bytes(response[:header_end]), bytearray(response[header_end:])


def _parse_handshake(response: bytes, key: str) -> None:
    lines = response.decode("latin-1").split("\r\n")
    status_fields = lines[0].split(" ", 2)
    if len(status_fields) < 2 or status_fields[1] != "101":
        raise RuntimeError(f"WebSocket handshake failed: {lines[0]}")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if not line:
            continue
        if ":" not in line:
            raise RuntimeError(f"invalid WebSocket response header: {line!r}")
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()

    expected_accept = base64.b64encode(
        hashlib.sha1((key + WEBSOCKET_GUID).encode("ascii")).digest()
    ).decode("ascii")
    if headers.get("sec-websocket-accept") != expected_accept:
        raise RuntimeError("WebSocket handshake returned an invalid accept key")
    if headers.get("upgrade", "").lower() != "websocket":
        raise RuntimeError("WebSocket handshake did not upgrade the connection")
    connection_tokens = {
        token.strip().lower() for token in headers.get("connection", "").split(",")
    }
    if "upgrade" not in connection_tokens:
        raise RuntimeError("WebSocket handshake omitted Connection: Upgrade")


def _encode_client_frame(payload: bytes, opcode: int = 0x1) -> bytes:
    mask = os.urandom(4)
    length = len(payload)
    if length < 126:
        header = bytes((0x80 | opcode, 0x80 | length))
    elif length <= 0xFFFF:
        header = bytes((0x80 | opcode, 0x80 | 126)) + struct.pack("!H", length)
    else:
        header = bytes((0x80 | opcode, 0x80 | 127)) + struct.pack("!Q", length)
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    return header + mask + masked


class WebSocketConnection:
    def __init__(self, connection: socket.socket, buffered: bytearray) -> None:
        self._connection = connection
        self._buffer = buffered

    @classmethod
    def connect(
        cls, host: str, port: int, path: str, timeout: float
    ) -> "WebSocketConnection":
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        ).encode("ascii")
        connection = socket.create_connection((host, port), timeout=timeout)
        try:
            connection.settimeout(timeout)
            connection.sendall(request)
            response, buffered = _read_handshake(connection)
            _parse_handshake(response, key)
            return cls(connection, buffered)
        except Exception:
            connection.close()
            raise

    def __enter__(self) -> "WebSocketConnection":
        return self

    def __exit__(self, *_: object) -> None:
        self._connection.close()

    def _read_exact(self, length: int, deadline: float) -> bytes:
        while len(self._buffer) < length:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for a CDP WebSocket message")
            self._connection.settimeout(remaining)
            try:
                block = self._connection.recv(max(4096, length - len(self._buffer)))
            except socket.timeout as exc:
                raise TimeoutError(
                    "timed out waiting for a CDP WebSocket message"
                ) from exc
            if not block:
                raise RuntimeError("CDP WebSocket closed unexpectedly")
            self._buffer.extend(block)
            if len(self._buffer) > MAX_MESSAGE_BYTES + 14:
                raise RuntimeError("CDP WebSocket message exceeded the size limit")
        result = bytes(self._buffer[:length])
        del self._buffer[:length]
        return result

    def _read_frame(self, deadline: float) -> tuple[bool, int, bytes]:
        first, second = self._read_exact(2, deadline)
        if first & 0x70:
            raise RuntimeError("CDP WebSocket frame used unsupported RSV bits")
        finished = bool(first & 0x80)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._read_exact(2, deadline))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._read_exact(8, deadline))[0]
        if length > MAX_MESSAGE_BYTES:
            raise RuntimeError("CDP WebSocket message exceeded the size limit")
        mask = self._read_exact(4, deadline) if masked else b""
        payload = self._read_exact(length, deadline)
        if masked:
            payload = bytes(
                value ^ mask[index % 4] for index, value in enumerate(payload)
            )
        return finished, opcode, payload

    def send_json(self, message: dict[str, Any]) -> None:
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        self._connection.sendall(_encode_client_frame(payload))

    def receive_json(self, deadline: float) -> dict[str, Any]:
        fragments = bytearray()
        message_started = False
        while True:
            finished, opcode, payload = self._read_frame(deadline)
            if opcode == 0x8:
                raise RuntimeError("CDP WebSocket closed before returning a response")
            if opcode == 0x9:
                self._connection.sendall(_encode_client_frame(payload, opcode=0xA))
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x1:
                if message_started:
                    raise RuntimeError("CDP WebSocket started overlapping messages")
                message_started = True
                fragments.extend(payload)
            elif opcode == 0x0 and message_started:
                fragments.extend(payload)
            else:
                raise RuntimeError(f"unsupported CDP WebSocket opcode: {opcode}")
            if len(fragments) > MAX_MESSAGE_BYTES:
                raise RuntimeError("CDP WebSocket message exceeded the size limit")
            if not finished:
                continue
            try:
                message = json.loads(fragments.decode("utf-8"))
            except (UnicodeError, json.JSONDecodeError) as exc:
                raise RuntimeError("CDP returned an invalid JSON message") from exc
            if not isinstance(message, dict):
                raise RuntimeError("CDP returned a non-object JSON message")
            return message


class CdpClient:
    def __init__(self, websocket: WebSocketConnection) -> None:
        self._websocket = websocket
        self._next_id = 1

    def send_command(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        *,
        session_id: str | None = None,
    ) -> int:
        command_id = self._next_id
        self._next_id += 1
        message: dict[str, Any] = {"id": command_id, "method": method}
        if params is not None:
            message["params"] = params
        if session_id is not None:
            message["sessionId"] = session_id
        self._websocket.send_json(message)
        return command_id

    def call(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        *,
        session_id: str | None = None,
        deadline: float,
    ) -> dict[str, Any]:
        command_id = self.send_command(method, params, session_id=session_id)
        while True:
            message = self._websocket.receive_json(deadline)
            if message.get("id") != command_id:
                continue
            if "error" in message:
                error = message["error"]
                raise RuntimeError(f"CDP {method} failed: {error}")
            result = message.get("result", {})
            if not isinstance(result, dict):
                raise RuntimeError(f"CDP {method} returned an invalid result")
            return result


def _normalize_url(value: str) -> str:
    return value[:-1] if value.endswith("/") else value


def _require_mapping(value: object, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise RuntimeError(f"{context} was not an object")
    return value


def _require_string(value: object, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise RuntimeError(f"{context} was missing or empty")
    return value


def _remaining(deadline: float, context: str) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError(f"timed out {context}")
    return remaining


def verify_limitless_page(
    client: CdpClient,
    *,
    expected_url: str,
    expected_build_id: str,
    expected_revision: str,
    timeout: float,
    poll_interval: float = 0.1,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    version = client.call("Browser.getVersion", deadline=deadline)
    browser_revision = _require_string(
        version.get("revision"), "Browser.getVersion revision"
    )
    normalized_browser_revision = browser_revision.removeprefix("@")
    if normalized_browser_revision == expected_revision:
        browser_revision_status = "exact"
    elif normalized_browser_revision == UNVERSIONED_LOCAL_REVISION:
        browser_revision_status = "unversioned-local"
    else:
        raise RuntimeError(
            "live browser revision mismatch: "
            f"expected {expected_revision} or the 40-zero dummy LASTCHANGE value, "
            f"found {browser_revision}"
        )

    target_info: dict[str, Any] | None = None
    seen_targets: set[str] = set()
    while target_info is None:
        targets = client.call("Target.getTargets", deadline=deadline).get(
            "targetInfos", []
        )
        if not isinstance(targets, list):
            raise RuntimeError("Target.getTargets returned an invalid target list")
        for raw_target in targets:
            if not isinstance(raw_target, dict):
                continue
            target_url = raw_target.get("url")
            target_type = raw_target.get("type")
            if isinstance(target_url, str):
                seen_targets.add(f"{target_type or 'unknown'}:{target_url}")
                if _normalize_url(target_url) == _normalize_url(expected_url):
                    target_info = raw_target
                    break
        if target_info is not None:
            break
        remaining = _remaining(
            deadline,
            f"waiting for the live {expected_url} CDP target; "
            f"observed {sorted(seen_targets)}",
        )
        time.sleep(min(poll_interval, remaining))

    target_id = _require_string(target_info.get("targetId"), "Limitless target ID")
    target_url = _require_string(target_info.get("url"), "Limitless target URL")
    attached = client.call(
        "Target.attachToTarget",
        {"targetId": target_id, "flatten": True},
        deadline=deadline,
    )
    session_id = _require_string(attached.get("sessionId"), "Limitless CDP session ID")
    client.call("Runtime.enable", session_id=session_id, deadline=deadline)

    last_state: dict[str, Any] = {}
    while True:
        evaluated = client.call(
            "Runtime.evaluate",
            {
                "expression": SHELL_STATE_EXPRESSION,
                "returnByValue": True,
                "awaitPromise": True,
            },
            session_id=session_id,
            deadline=deadline,
        )
        if "exceptionDetails" in evaluated:
            raise RuntimeError(
                f"Limitless readiness evaluation failed: {evaluated['exceptionDetails']}"
            )
        remote_object = _require_mapping(
            evaluated.get("result"), "Runtime.evaluate result"
        )
        last_state = _require_mapping(
            remote_object.get("value"), "Limitless page state"
        )
        if last_state.get("ready") is True:
            break
        remaining = _remaining(
            deadline,
            f"waiting for #limitless-shell[data-ready=true]; last state {last_state}",
        )
        time.sleep(min(poll_interval, remaining))

    if last_state.get("shellPresent") is not True:
        raise RuntimeError(
            "live Limitless page reported ready without #limitless-shell"
        )
    if last_state.get("markerPresent") is not True:
        raise RuntimeError("live Limitless page is missing #limitless-build-marker")
    page_url = _require_string(last_state.get("url"), "live Limitless page URL")
    if _normalize_url(page_url) != _normalize_url(expected_url):
        raise RuntimeError(
            f"live Limitless page URL mismatch: expected {expected_url}, found {page_url}"
        )
    build_id = _require_string(last_state.get("buildId"), "Limitless build ID")
    if build_id != expected_build_id:
        raise RuntimeError(
            f"Limitless build ID mismatch: expected {expected_build_id}, found {build_id}"
        )
    marker_revision = _require_string(
        last_state.get("chromiumRevision"), "Limitless marker Chromium revision"
    )
    if marker_revision != expected_revision:
        raise RuntimeError(
            "Limitless marker Chromium revision mismatch: "
            f"expected {expected_revision}, found {marker_revision}"
        )

    return {
        "build_id": build_id,
        "browser_revision": browser_revision,
        "browser_revision_status": browser_revision_status,
        "chromium_revision": marker_revision,
        "ready": True,
        "target_id": target_id,
        "target_url": target_url,
    }


def validate_then_close(
    client: CdpClient,
    *,
    expected_url: str,
    expected_build_id: str,
    expected_revision: str,
    timeout: float,
    poll_interval: float = 0.1,
) -> dict[str, Any]:
    validation_error: Exception | None = None
    result: dict[str, Any] | None = None
    try:
        result = verify_limitless_page(
            client,
            expected_url=expected_url,
            expected_build_id=expected_build_id,
            expected_revision=expected_revision,
            timeout=timeout,
            poll_interval=poll_interval,
        )
    except Exception as exc:
        validation_error = exc

    try:
        client.send_command("Browser.close")
    except Exception as close_error:
        if validation_error is None:
            raise RuntimeError(
                f"could not request Browser.close after validation: {close_error}"
            ) from close_error

    if validation_error is not None:
        raise validation_error
    if result is None:
        raise RuntimeError("Limitless validation produced no result")
    return result


def validate_profile(
    profile: Path,
    *,
    expected_url: str,
    expected_build_id: str,
    expected_revision: str,
    timeout: float,
) -> dict[str, Any]:
    port, websocket_path = read_debugger_endpoint(profile)
    with WebSocketConnection.connect(
        "127.0.0.1", port, websocket_path, timeout
    ) as websocket:
        return validate_then_close(
            CdpClient(websocket),
            expected_url=expected_url,
            expected_build_id=expected_build_id,
            expected_revision=expected_revision,
            timeout=timeout,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--url", default=DEFAULT_SHELL_URL)
    parser.add_argument("--expected-build-id", required=True)
    parser.add_argument("--expected-chromium-revision", required=True)
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if not args.expected_build_id.strip():
        parser.error("--expected-build-id must be non-empty")
    if not REVISION_RE.fullmatch(args.expected_chromium_revision):
        parser.error(
            "--expected-chromium-revision must be a lowercase 40-character SHA"
        )

    try:
        result = validate_profile(
            args.profile,
            expected_url=args.url,
            expected_build_id=args.expected_build_id,
            expected_revision=args.expected_chromium_revision,
            timeout=args.timeout,
        )
    except (OSError, RuntimeError, TimeoutError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
