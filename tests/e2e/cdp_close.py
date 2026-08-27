#!/usr/bin/env python3
"""Validate the live Limitless shell through CDP, then close the browser."""

from __future__ import annotations

import argparse
import base64
import binascii
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import socket
import struct
import sys
import threading
import time
import zlib
from pathlib import Path
from socketserver import TCPServer
from typing import Any, Callable, Iterator
from urllib.parse import urlsplit


WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_HANDSHAKE_BYTES = 65536
MAX_MESSAGE_BYTES = 16 * 1024 * 1024
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
UNVERSIONED_LOCAL_REVISION = "0" * 40
DEFAULT_SHELL_URL = "chrome://webui-browser/"
NAVIGATION_PATH = "/limitless-gate2-navigation.html"
NAVIGATION_TITLE = "Limitless Gate 2 Navigation"
NAVIGATION_CHECKPOINT = "address-control-local-page"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

NAVIGATION_PAGE = f"""<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>{NAVIGATION_TITLE}</title>
    <style>
      :root {{ color-scheme: light dark; font-family: system-ui, sans-serif; }}
      body {{ display: grid; min-height: 100vh; margin: 0; place-items: center; }}
      main {{ max-width: 42rem; padding: 3rem; text-align: center; }}
      h1 {{ font-size: 2rem; margin: 0 0 1rem; }}
    </style>
  </head>
  <body>
    <main id="limitless-navigation-checkpoint"
        data-checkpoint="{NAVIGATION_CHECKPOINT}">
      <h1>{NAVIGATION_TITLE}</h1>
      <p>The live address control navigated a real browser tab here.</p>
    </main>
  </body>
</html>
""".encode(
    "utf-8"
)

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

ADDRESS_STATE_EXPRESSION = r"""
(() => {
  const app = document.querySelector('webui-browser-app');
  const address = app?.shadowRoot?.querySelector('#address');
  const searchboxInput = address?.shadowRoot?.querySelector('#input');
  const input = searchboxInput?.shadowRoot?.querySelector('#input');
  const bounds = input instanceof HTMLInputElement
      ? input.getBoundingClientRect()
      : null;
  const describe = element => element instanceof Element
      ? `${element.tagName.toLowerCase()}#${element.id}`
      : '';
  const centerX = bounds !== null ? bounds.left + bounds.width / 2 : 0;
  const centerY = bounds !== null ? bounds.top + bounds.height / 2 : 0;
  return {
    addressPresent: address instanceof HTMLElement,
    inputPresent: input instanceof HTMLInputElement,
    clickable: bounds !== null && bounds.width > 0 && bounds.height > 0,
    boundsLeft: bounds !== null ? bounds.left : 0,
    boundsTop: bounds !== null ? bounds.top : 0,
    boundsWidth: bounds !== null ? bounds.width : 0,
    boundsHeight: bounds !== null ? bounds.height : 0,
    centerX,
    centerY,
    documentHit: describe(document.elementFromPoint(centerX, centerY)),
    appHit: describe(app?.shadowRoot?.elementFromPoint(centerX, centerY)),
    addressHit: describe(address?.shadowRoot?.elementFromPoint(centerX, centerY)),
    searchboxHit: describe(
        searchboxInput?.shadowRoot?.elementFromPoint(centerX, centerY)),
    focused: document.activeElement === app &&
        app?.shadowRoot?.activeElement === address &&
        address?.shadowRoot?.activeElement === searchboxInput &&
        searchboxInput?.shadowRoot?.activeElement === input,
    documentActive: describe(document.activeElement),
    appActive: describe(app?.shadowRoot?.activeElement),
    addressActive: describe(address?.shadowRoot?.activeElement),
    searchboxActive: describe(searchboxInput?.shadowRoot?.activeElement),
    inputMatchesFocus: input instanceof HTMLInputElement && input.matches(':focus'),
    value: input instanceof HTMLInputElement ? input.value : '',
  };
})()
""".strip()

NAVIGATION_STATE_EXPRESSION = r"""
(() => {
  const marker = document.querySelector('#limitless-navigation-checkpoint');
  return {
    url: location.href,
    ready: document.readyState === 'complete',
    markerPresent: marker instanceof HTMLElement,
    checkpoint: marker instanceof HTMLElement ? marker.dataset.checkpoint ?? '' : '',
    title: document.title,
  };
})()
""".strip()


class _NavigationCheckpointHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if urlsplit(self.path).path != NAVIGATION_PATH:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(NAVIGATION_PAGE)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(NAVIGATION_PAGE)

    def log_message(self, _format: str, *args: object) -> None:
        del args


class _LoopbackThreadingHTTPServer(ThreadingHTTPServer):
    def server_bind(self) -> None:
        # HTTPServer performs a reverse-DNS lookup while binding. Gate 2 needs a
        # deterministic loopback-only server and must not depend on DNS.
        TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = int(self.server_address[1])


@contextmanager
def serve_navigation_checkpoint() -> Iterator[str]:
    server = _LoopbackThreadingHTTPServer(
        ("127.0.0.1", 0), _NavigationCheckpointHandler
    )
    server.daemon_threads = True
    port = int(server.server_address[1])
    thread = threading.Thread(
        target=server.serve_forever,
        name="limitless-navigation-checkpoint",
        daemon=True,
    )
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}{NAVIGATION_PATH}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        if thread.is_alive():
            raise RuntimeError("local navigation checkpoint server did not stop")


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
            try:
                message = self._websocket.receive_json(deadline)
            except TimeoutError as exc:
                raise TimeoutError(
                    f"timed out waiting for CDP {method} response"
                ) from exc
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


def _decode_png_dimensions(
    png: bytes, *, require_nontransparent: bool = False
) -> tuple[int, int]:
    if not png.startswith(PNG_SIGNATURE):
        raise RuntimeError("Page.captureScreenshot did not return a PNG")

    offset = len(PNG_SIGNATURE)
    width = 0
    height = 0
    bit_depth = 0
    color_type = -1
    interlace_method = -1
    idat = bytearray()
    saw_ihdr = False
    saw_iend = False
    while offset < len(png):
        if len(png) - offset < 12:
            raise RuntimeError("Page.captureScreenshot returned a truncated PNG chunk")
        chunk_length = struct.unpack("!I", png[offset : offset + 4])[0]
        chunk_type = png[offset + 4 : offset + 8]
        chunk_end = offset + 12 + chunk_length
        if chunk_end > len(png):
            raise RuntimeError("Page.captureScreenshot returned a truncated PNG chunk")
        chunk_data = png[offset + 8 : offset + 8 + chunk_length]
        expected_crc = struct.unpack("!I", png[offset + 8 + chunk_length : chunk_end])[
            0
        ]
        actual_crc = binascii.crc32(chunk_type + chunk_data) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise RuntimeError("Page.captureScreenshot returned a PNG with a bad CRC")
        if chunk_type == b"IHDR":
            if saw_ihdr or offset != len(PNG_SIGNATURE) or chunk_length != 13:
                raise RuntimeError(
                    "Page.captureScreenshot returned an invalid PNG IHDR"
                )
            (
                width,
                height,
                bit_depth,
                color_type,
                compression_method,
                filter_method,
                interlace_method,
            ) = struct.unpack("!IIBBBBB", chunk_data)
            if (
                width <= 0
                or height <= 0
                or compression_method != 0
                or filter_method != 0
            ):
                raise RuntimeError(
                    "Page.captureScreenshot returned an invalid PNG IHDR"
                )
            saw_ihdr = True
        elif chunk_type == b"IDAT":
            if not saw_ihdr or saw_iend:
                raise RuntimeError("Page.captureScreenshot returned invalid PNG chunks")
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            if chunk_length != 0 or not saw_ihdr or saw_iend:
                raise RuntimeError(
                    "Page.captureScreenshot returned an invalid PNG IEND"
                )
            saw_iend = True
            if chunk_end != len(png):
                raise RuntimeError("Page.captureScreenshot returned trailing PNG data")
        offset = chunk_end

    if not saw_ihdr or not saw_iend or not idat:
        raise RuntimeError("Page.captureScreenshot returned an incomplete PNG")
    channels_by_color_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    valid_bit_depths = {
        0: {1, 2, 4, 8, 16},
        2: {8, 16},
        3: {1, 2, 4, 8},
        4: {8, 16},
        6: {8, 16},
    }
    if (
        color_type not in channels_by_color_type
        or bit_depth not in valid_bit_depths[color_type]
        or interlace_method != 0
    ):
        raise RuntimeError("Page.captureScreenshot returned an unsupported PNG format")

    decompressor = zlib.decompressobj()
    try:
        filtered = decompressor.decompress(bytes(idat)) + decompressor.flush()
    except zlib.error as exc:
        raise RuntimeError(
            "Page.captureScreenshot returned corrupt PNG pixels"
        ) from exc
    if not decompressor.eof or decompressor.unused_data:
        raise RuntimeError("Page.captureScreenshot returned corrupt PNG pixels")

    channels = channels_by_color_type[color_type]
    bits_per_pixel = channels * bit_depth
    stride = (width * bits_per_pixel + 7) // 8
    bytes_per_pixel = max(1, (bits_per_pixel + 7) // 8)
    expected_size = height * (stride + 1)
    if len(filtered) != expected_size:
        raise RuntimeError("Page.captureScreenshot returned invalid PNG dimensions")

    previous = bytearray(stride)
    saw_nontransparent_pixel = not require_nontransparent
    cursor = 0
    for _ in range(height):
        filter_type = filtered[cursor]
        encoded_row = filtered[cursor + 1 : cursor + 1 + stride]
        cursor += stride + 1
        if filter_type > 4:
            raise RuntimeError("Page.captureScreenshot returned an invalid PNG filter")
        decoded_row = bytearray(stride)
        for index, encoded_byte in enumerate(encoded_row):
            left = (
                decoded_row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
            )
            above = previous[index]
            upper_left = (
                previous[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
            )
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            else:
                estimate = left + above - upper_left
                distance_left = abs(estimate - left)
                distance_above = abs(estimate - above)
                distance_upper_left = abs(estimate - upper_left)
                predictor = (
                    left
                    if distance_left <= distance_above
                    and distance_left <= distance_upper_left
                    else above
                    if distance_above <= distance_upper_left
                    else upper_left
                )
            decoded_row[index] = (encoded_byte + predictor) & 0xFF
        if require_nontransparent and not saw_nontransparent_pixel:
            if color_type == 6 and bit_depth == 8:
                saw_nontransparent_pixel = any(decoded_row[3::4])
            elif color_type == 6 and bit_depth == 16:
                saw_nontransparent_pixel = any(
                    decoded_row[index]
                    for index in range(6, len(decoded_row), 8)
                ) or any(
                    decoded_row[index]
                    for index in range(7, len(decoded_row), 8)
                )
            elif color_type == 4 and bit_depth == 8:
                saw_nontransparent_pixel = any(decoded_row[1::2])
            elif color_type == 4 and bit_depth == 16:
                saw_nontransparent_pixel = any(
                    decoded_row[index]
                    for index in range(2, len(decoded_row), 4)
                ) or any(
                    decoded_row[index]
                    for index in range(3, len(decoded_row), 4)
                )
            else:
                saw_nontransparent_pixel = True
        previous = decoded_row
    if not saw_nontransparent_pixel:
        raise RuntimeError("Page.captureScreenshot returned a fully transparent PNG")
    return width, height


def _remaining(deadline: float, context: str) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError(f"timed out {context}")
    return remaining


def _evaluate_mapping(
    client: CdpClient,
    *,
    expression: str,
    session_id: str,
    deadline: float,
    context: str,
) -> dict[str, Any]:
    try:
        evaluated = client.call(
            "Runtime.evaluate",
            {
                "expression": expression,
                "returnByValue": True,
                "awaitPromise": True,
            },
            session_id=session_id,
            deadline=deadline,
        )
    except TimeoutError as exc:
        raise TimeoutError(f"timed out evaluating {context}") from exc
    if "exceptionDetails" in evaluated:
        raise RuntimeError(
            f"{context} evaluation failed: {evaluated['exceptionDetails']}"
        )
    remote_object = _require_mapping(
        evaluated.get("result"), f"{context} Runtime.evaluate result"
    )
    return _require_mapping(remote_object.get("value"), context)


def _dispatch_key(
    client: CdpClient,
    *,
    session_id: str,
    key: str,
    code: str,
    windows_virtual_key_code: int,
    native_virtual_key_code: int,
    modifiers: int,
    deadline: float,
) -> None:
    params = {
        "key": key,
        "code": code,
        "windowsVirtualKeyCode": windows_virtual_key_code,
        "nativeVirtualKeyCode": native_virtual_key_code,
        "modifiers": modifiers,
    }
    client.call(
        "Input.dispatchKeyEvent",
        {"type": "rawKeyDown", **params},
        session_id=session_id,
        deadline=deadline,
    )
    client.call(
        "Input.dispatchKeyEvent",
        {"type": "keyUp", **params},
        session_id=session_id,
        deadline=deadline,
    )


def _wait_for_address_state(
    client: CdpClient,
    *,
    session_id: str,
    deadline: float,
    poll_interval: float,
    description: str,
    predicate: Callable[[dict[str, Any]], bool],
) -> dict[str, Any]:
    last_state: dict[str, Any] = {}
    while True:
        _remaining(
            deadline,
            f"waiting for {description}; last address state {last_state}",
        )
        last_state = _evaluate_mapping(
            client,
            expression=ADDRESS_STATE_EXPRESSION,
            session_id=session_id,
            deadline=deadline,
            context=f"Limitless address control state while waiting for {description}",
        )
        if predicate(last_state):
            return last_state
        remaining = _remaining(
            deadline,
            f"waiting for {description}; last address state {last_state}",
        )
        time.sleep(min(poll_interval, remaining))


def navigate_via_address_control(
    client: CdpClient,
    *,
    shell_session_id: str,
    shell_target_id: str,
    navigation_url: str,
    timeout: float,
    poll_interval: float = 0.1,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    initial_state = _wait_for_address_state(
        client,
        session_id=shell_session_id,
        deadline=deadline,
        poll_interval=poll_interval,
        description="the live WebUI Browser address control",
        predicate=lambda state: state.get("inputPresent") is True
        and state.get("clickable") is True
        and state.get("appHit") == "webui-browser-searchbox#address"
        and state.get("addressHit") == "cr-searchbox-input#input"
        and state.get("searchboxHit") == "input#input",
    )

    pre_navigation_targets = client.call("Target.getTargets", deadline=deadline).get(
        "targetInfos", []
    )
    if not isinstance(pre_navigation_targets, list):
        raise RuntimeError("Target.getTargets returned an invalid target list")
    pre_navigation_tabs = [
        target
        for target in pre_navigation_targets
        if isinstance(target, dict)
        and target.get("type") == "page"
        and target.get("targetId") != shell_target_id
    ]
    if len(pre_navigation_tabs) != 1:
        observed = [
            f"{target.get('type', 'unknown')}:{target.get('targetId', '')}:"
            f"{target.get('url', '')}"
            for target in pre_navigation_targets
            if isinstance(target, dict)
        ]
        raise RuntimeError(
            "expected exactly one real tab before address navigation; "
            f"found {len(pre_navigation_tabs)} in {observed}"
        )
    pre_navigation_target_id = _require_string(
        pre_navigation_tabs[0].get("targetId"), "pre-navigation real tab target ID"
    )
    pre_navigation_url = _require_string(
        pre_navigation_tabs[0].get("url"), "pre-navigation real tab URL"
    )

    center_x = initial_state.get("centerX")
    center_y = initial_state.get("centerY")
    if not isinstance(center_x, (int, float)) or not isinstance(center_y, (int, float)):
        raise RuntimeError("live address input did not expose clickable coordinates")
    client.call(
        "Input.dispatchMouseEvent",
        {
            "type": "mouseMoved",
            "x": center_x,
            "y": center_y,
            "button": "none",
        },
        session_id=shell_session_id,
        deadline=deadline,
    )
    for event_type in ("mousePressed", "mouseReleased"):
        params: dict[str, Any] = {
            "type": event_type,
            "x": center_x,
            "y": center_y,
            "button": "left",
            "clickCount": 1,
        }
        client.call(
            "Input.dispatchMouseEvent",
            params,
            session_id=shell_session_id,
            deadline=deadline,
        )
    _wait_for_address_state(
        client,
        session_id=shell_session_id,
        deadline=deadline,
        poll_interval=poll_interval,
        description="a coordinate click to focus the live address input",
        predicate=lambda state: state.get("focused") is True,
    )

    client.call(
        "Input.insertText",
        {"text": navigation_url},
        session_id=shell_session_id,
        deadline=deadline,
    )
    _wait_for_address_state(
        client,
        session_id=shell_session_id,
        deadline=deadline,
        poll_interval=poll_interval,
        description=f"the address input to contain {navigation_url}",
        predicate=lambda state: state.get("value") == navigation_url,
    )

    _dispatch_key(
        client,
        session_id=shell_session_id,
        key="Enter",
        code="Enter",
        windows_virtual_key_code=13,
        native_virtual_key_code=36,
        modifiers=0,
        deadline=deadline,
    )

    target_info: dict[str, Any] | None = None
    seen_targets: set[str] = set()
    while target_info is None:
        targets = client.call("Target.getTargets", deadline=deadline).get(
            "targetInfos", []
        )
        if not isinstance(targets, list):
            raise RuntimeError("Target.getTargets returned an invalid target list")
        real_tabs = [
            target
            for target in targets
            if isinstance(target, dict)
            and target.get("type") == "page"
            and target.get("targetId") != shell_target_id
        ]
        if len(real_tabs) != 1:
            raise RuntimeError(
                "expected exactly one real tab after address submission; "
                f"found {len(real_tabs)}"
            )
        raw_target = real_tabs[0]
        current_target_id = _require_string(
            raw_target.get("targetId"), "post-submission real tab target ID"
        )
        if current_target_id != pre_navigation_target_id:
            raise RuntimeError(
                "address submission replaced or opened a different real tab: "
                f"expected {pre_navigation_target_id}, found {current_target_id}"
            )
        for raw_target in real_tabs:
            target_url = raw_target.get("url")
            target_type = raw_target.get("type")
            if isinstance(target_url, str):
                seen_targets.add(f"{target_type or 'unknown'}:{target_url}")
            if (
                target_type == "page"
                and isinstance(target_url, str)
                and _normalize_url(target_url) == _normalize_url(navigation_url)
            ):
                target_info = raw_target
                break
        if target_info is not None:
            break
        remaining = _remaining(
            deadline,
            f"waiting for the real tab target to navigate to {navigation_url}; "
            f"observed {sorted(seen_targets)}",
        )
        time.sleep(min(poll_interval, remaining))

    target_id = _require_string(target_info.get("targetId"), "navigated tab target ID")
    attached = client.call(
        "Target.attachToTarget",
        {"targetId": target_id, "flatten": True},
        deadline=deadline,
    )
    tab_session_id = _require_string(
        attached.get("sessionId"), "navigated tab CDP session ID"
    )
    client.call("Runtime.enable", session_id=tab_session_id, deadline=deadline)

    last_state: dict[str, Any] = {}
    while True:
        last_state = _evaluate_mapping(
            client,
            expression=NAVIGATION_STATE_EXPRESSION,
            session_id=tab_session_id,
            deadline=deadline,
            context="navigated tab state",
        )
        if last_state.get("ready") is True and last_state.get("markerPresent") is True:
            break
        remaining = _remaining(
            deadline,
            "waiting for #limitless-navigation-checkpoint in the real tab; "
            f"last state {last_state}",
        )
        time.sleep(min(poll_interval, remaining))

    navigated_url = _require_string(last_state.get("url"), "navigated tab URL")
    if _normalize_url(navigated_url) != _normalize_url(navigation_url):
        raise RuntimeError(
            f"navigated tab URL mismatch: expected {navigation_url}, "
            f"found {navigated_url}"
        )
    checkpoint = _require_string(last_state.get("checkpoint"), "navigation checkpoint")
    if checkpoint != NAVIGATION_CHECKPOINT:
        raise RuntimeError(
            "navigation checkpoint mismatch: "
            f"expected {NAVIGATION_CHECKPOINT}, found {checkpoint}"
        )
    title = _require_string(last_state.get("title"), "navigated tab title")
    if title != NAVIGATION_TITLE:
        raise RuntimeError(
            f"navigated tab title mismatch: expected {NAVIGATION_TITLE}, found {title}"
        )

    return {
        "address_hit_test": {
            "address": initial_state.get("addressHit"),
            "app": initial_state.get("appHit"),
            "input": initial_state.get("searchboxHit"),
        },
        "address_input_method": "cdp-coordinate-click-insert-text-enter",
        "navigation_checkpoint": checkpoint,
        "navigation_target_id": target_id,
        "navigation_title": title,
        "navigation_url": navigated_url,
        "pre_navigation_target_id": pre_navigation_target_id,
        "pre_navigation_url": pre_navigation_url,
        "real_tab_count": 1,
    }


def capture_shell_renderer_screenshot(
    client: CdpClient,
    *,
    shell_session_id: str,
    screenshot_path: Path,
    timeout: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    client.call("Page.enable", session_id=shell_session_id, deadline=deadline)
    capture = client.call(
        "Page.captureScreenshot",
        {
            "format": "png",
            "fromSurface": True,
            "captureBeyondViewport": False,
        },
        session_id=shell_session_id,
        deadline=deadline,
    )
    encoded = _require_string(capture.get("data"), "Page.captureScreenshot data")
    try:
        screenshot = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise RuntimeError("Page.captureScreenshot returned invalid base64") from exc
    width, height = _decode_png_dimensions(screenshot)
    screenshot_path.parent.mkdir(parents=True, exist_ok=True)
    screenshot_path.write_bytes(screenshot)
    return {
        "shell_renderer_screenshot": str(screenshot_path),
        "shell_renderer_screenshot_bytes": len(screenshot),
        "shell_renderer_screenshot_height": height,
        "shell_renderer_screenshot_sha256": hashlib.sha256(screenshot).hexdigest(),
        "shell_renderer_screenshot_width": width,
    }


def verify_limitless_page(
    client: CdpClient,
    *,
    expected_url: str,
    expected_build_id: str,
    expected_revision: str,
    timeout: float,
    poll_interval: float = 0.1,
) -> tuple[dict[str, Any], str]:
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
        last_state = _evaluate_mapping(
            client,
            expression=SHELL_STATE_EXPRESSION,
            session_id=session_id,
            deadline=deadline,
            context="Limitless page state",
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

    return (
        {
            "build_id": build_id,
            "browser_revision": browser_revision,
            "browser_revision_status": browser_revision_status,
            "chromium_revision": marker_revision,
            "ready": True,
            "target_id": target_id,
            "target_url": target_url,
        },
        session_id,
    )


def validate_then_close(
    client: CdpClient,
    *,
    expected_url: str,
    expected_build_id: str,
    expected_revision: str,
    navigation_url: str,
    screenshot_path: Path,
    timeout: float,
    poll_interval: float = 0.1,
) -> dict[str, Any]:
    validation_error: Exception | None = None
    result: dict[str, Any] | None = None
    try:
        result, shell_session_id = verify_limitless_page(
            client,
            expected_url=expected_url,
            expected_build_id=expected_build_id,
            expected_revision=expected_revision,
            timeout=timeout,
            poll_interval=poll_interval,
        )
        pre_navigation_screenshot = screenshot_path.with_name(
            f"{screenshot_path.stem}-pre-navigation{screenshot_path.suffix}"
        )
        pre_navigation_capture = capture_shell_renderer_screenshot(
            client,
            shell_session_id=shell_session_id,
            screenshot_path=pre_navigation_screenshot,
            timeout=timeout,
        )
        result["pre_navigation_renderer_screenshot"] = pre_navigation_capture[
            "shell_renderer_screenshot"
        ]
        result.update(
            navigate_via_address_control(
                client,
                shell_session_id=shell_session_id,
                shell_target_id=_require_string(
                    result.get("target_id"), "Limitless shell target ID"
                ),
                navigation_url=navigation_url,
                timeout=timeout,
                poll_interval=poll_interval,
            )
        )
        result.update(
            capture_shell_renderer_screenshot(
                client,
                shell_session_id=shell_session_id,
                screenshot_path=screenshot_path,
                timeout=timeout,
            )
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
    screenshot_path: Path,
    timeout: float,
) -> dict[str, Any]:
    port, websocket_path = read_debugger_endpoint(profile)
    with serve_navigation_checkpoint() as navigation_url:
        with WebSocketConnection.connect(
            "127.0.0.1", port, websocket_path, timeout
        ) as websocket:
            return validate_then_close(
                CdpClient(websocket),
                expected_url=expected_url,
                expected_build_id=expected_build_id,
                expected_revision=expected_revision,
                navigation_url=navigation_url,
                screenshot_path=screenshot_path,
                timeout=timeout,
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--url", default=DEFAULT_SHELL_URL)
    parser.add_argument("--screenshot", required=True, type=Path)
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
            screenshot_path=args.screenshot,
            timeout=args.timeout,
        )
    except (OSError, RuntimeError, TimeoutError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
