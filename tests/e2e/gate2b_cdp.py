#!/usr/bin/env python3
"""Validate the live Gate 2B BrowserView boundary, then close the browser."""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable


sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cdp_close as cdp  # noqa: E402


DEFAULT_TAB_URL = "about:blank"
WEBUI_BROWSER_URL_PREFIX = "chrome://webui-browser"
RAIL_IDENTIFIER = "limitless-swiftui-boundary-rail"
FOCUS_IDENTIFIER_PREFIX = "limitless-tab-"
TAB_CLOSE_IDENTIFIER_PREFIX = "limitless-tab-close-"
EXPECTED_RAIL_WIDTH = 216.0
BOUNDS_TOLERANCE = 2.0
NATIVE_PROBE_CLEANUP_RESERVE = 1.0


def _require_mapping(value: object, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise RuntimeError(f"{context} was not an object")
    return value


def _require_string(value: object, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise RuntimeError(f"{context} was missing or empty")
    return value


def _require_number(value: object, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise RuntimeError(f"{context} was not numeric")
    number = float(value)
    if not math.isfinite(number):
        raise RuntimeError(f"{context} was not finite")
    return number


def _require_true(value: object, context: str) -> None:
    if value is not True:
        raise RuntimeError(f"{context} was not true")


def _require_false(value: object, context: str) -> None:
    if value is not False:
        raise RuntimeError(f"{context} was not false")


def _remaining(deadline: float, context: str) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError(f"timed out {context}")
    return remaining


def _validate_browser_revision(
    client: Any, *, expected_revision: str, deadline: float
) -> dict[str, str]:
    version = client.call("Browser.getVersion", deadline=deadline)
    product = _require_string(version.get("product"), "Browser.getVersion product")
    protocol_version = _require_string(
        version.get("protocolVersion"), "Browser.getVersion protocolVersion"
    )
    browser_revision = _require_string(
        version.get("revision"), "Browser.getVersion revision"
    )
    normalized_revision = browser_revision.removeprefix("@")
    if normalized_revision == expected_revision:
        revision_status = "exact"
    elif normalized_revision == cdp.UNVERSIONED_LOCAL_REVISION:
        revision_status = "unversioned-local"
    else:
        raise RuntimeError(
            "live browser revision mismatch: "
            f"expected {expected_revision} or the 40-zero dummy LASTCHANGE value, "
            f"found {browser_revision}"
        )
    return {
        "browser_product": product,
        "browser_protocol_version": protocol_version,
        "browser_revision": browser_revision,
        "browser_revision_status": revision_status,
    }


def _wait_for_single_real_page(
    client: Any,
    *,
    expected_url: str,
    deadline: float,
    poll_interval: float,
) -> dict[str, Any]:
    observed: list[str] = []
    while True:
        response = client.call("Target.getTargets", deadline=deadline)
        raw_targets = response.get("targetInfos", [])
        if not isinstance(raw_targets, list):
            raise RuntimeError("Target.getTargets returned an invalid target list")
        targets = [target for target in raw_targets if isinstance(target, dict)]
        observed = [
            f"{target.get('type', 'unknown')}:{target.get('targetId', '')}:"
            f"{target.get('url', '')}"
            for target in targets
        ]
        webui_targets = [
            target
            for target in targets
            if isinstance(target.get("url"), str)
            and str(target["url"]).startswith(WEBUI_BROWSER_URL_PREFIX)
        ]
        if webui_targets:
            raise RuntimeError(
                "Webium-disabled Gate 2B launch exposed a WebUI Browser shell target: "
                f"{observed}"
            )
        page_targets = [target for target in targets if target.get("type") == "page"]
        if len(page_targets) > 1:
            raise RuntimeError(
                "expected exactly one real page target; "
                f"found {len(page_targets)} in {observed}"
            )
        if len(page_targets) == 1:
            page_url = _require_string(page_targets[0].get("url"), "real page URL")
            if page_url != expected_url:
                raise RuntimeError(
                    f"real page URL mismatch: expected {expected_url}, found {page_url}"
                )
            return page_targets[0]
        remaining = _remaining(
            deadline,
            f"waiting for one real page target; observed {observed}",
        )
        time.sleep(min(poll_interval, remaining))


def _validate_window_bounds(
    bounds: dict[str, Any], *, context: str
) -> dict[str, float | str]:
    state = _require_string(bounds.get("windowState"), f"{context} state")
    if state != "normal":
        raise RuntimeError(f"{context} was not normal: {state}")
    normalized: dict[str, float | str] = {"windowState": state}
    for key in ("left", "top", "width", "height"):
        normalized[key] = _require_number(bounds.get(key), f"{context} {key}")
    if normalized["width"] <= 0 or normalized["height"] <= 0:
        raise RuntimeError(f"{context} had non-positive dimensions")
    return normalized


def _validate_native_probe(
    probe: dict[str, Any],
    *,
    browser_pid: int,
    screenshot_path: Path,
    cdp_bounds: dict[str, float | str],
) -> dict[str, Any]:
    _require_true(probe.get("screen_capture_permission"), "screen capture permission")
    _require_true(probe.get("accessibility_permission"), "accessibility permission")
    if probe.get("owner_pid") != browser_pid:
        raise RuntimeError(
            f"native window owner PID mismatch: expected {browser_pid}, "
            f"found {probe.get('owner_pid')}"
        )
    if probe.get("matching_window_count") != 1:
        raise RuntimeError("native probe did not resolve exactly one PID-owned window")
    if probe.get("ax_window_count") != 1:
        raise RuntimeError("native probe did not resolve exactly one AXWindow")
    if probe.get("rail_identifier") != RAIL_IDENTIFIER:
        raise RuntimeError("native probe did not expose the SwiftUI rail AX identifier")
    focus_identifier = _require_string(
        probe.get("focus_identifier"), "SwiftUI focus AX identifier"
    )
    if not focus_identifier.startswith(
        FOCUS_IDENTIFIER_PREFIX
    ) or focus_identifier.startswith(TAB_CLOSE_IDENTIFIER_PREFIX):
        raise RuntimeError(
            "native probe did not expose a focusable SwiftUI tab-row AX identifier"
        )
    _require_true(
        probe.get("rail_parent_reaches_window"),
        "SwiftUI rail AX parent chain to the native window",
    )
    _require_true(
        probe.get("focus_parent_reaches_window"),
        "SwiftUI focus AX parent chain to the native window",
    )
    _require_true(probe.get("focus_succeeded"), "SwiftUI native boundary focus")
    if probe.get("focus_method") != "macos-accessibility-AXFocused":
        raise RuntimeError("native probe reported an unexpected focus method")
    _require_false(probe.get("native_keyboard_input"), "native keyboard input")
    if probe.get("screenshot_method") != "screencapture-window-id":
        raise RuntimeError("native probe did not use exact-window native capture")

    rail_width = _require_number(probe.get("rail_width"), "SwiftUI rail width")
    if abs(rail_width - EXPECTED_RAIL_WIDTH) > BOUNDS_TOLERANCE:
        raise RuntimeError(
            f"SwiftUI rail width mismatch: expected {EXPECTED_RAIL_WIDTH}, "
            f"found {rail_width}"
        )

    native_bounds = _require_mapping(probe.get("window_bounds"), "native window bounds")
    normalized_native_bounds: dict[str, float] = {}
    for native_key, cdp_key in (
        ("x", "left"),
        ("y", "top"),
        ("width", "width"),
        ("height", "height"),
    ):
        native_value = _require_number(
            native_bounds.get(native_key), f"native window bounds {native_key}"
        )
        normalized_native_bounds[native_key] = native_value
        cdp_value = _require_number(cdp_bounds.get(cdp_key), f"CDP bounds {cdp_key}")
        if abs(native_value - cdp_value) > BOUNDS_TOLERANCE:
            raise RuntimeError(
                f"CDP/native window bounds mismatch for {cdp_key}: "
                f"CDP {cdp_value}, native {native_value}"
            )

    reported_screenshot = Path(
        _require_string(probe.get("screenshot"), "native screenshot path")
    ).resolve()
    if reported_screenshot != screenshot_path.resolve():
        raise RuntimeError(
            f"native screenshot path mismatch: expected {screenshot_path}, "
            f"found {reported_screenshot}"
        )
    try:
        png = screenshot_path.read_bytes()
    except OSError as exc:
        raise RuntimeError(f"could not read native window screenshot: {exc}") from exc
    try:
        png_width, png_height = cdp._decode_png_dimensions(
            png, require_nontransparent=True
        )
    except RuntimeError as exc:
        detail = str(exc).replace("Page.captureScreenshot", "native window screenshot")
        raise RuntimeError(detail) from exc
    display_scale = _require_number(probe.get("display_scale"), "display scale")
    supported_scale = next(
        (
            candidate
            for candidate in (1.0, 2.0)
            if math.isclose(display_scale, candidate, abs_tol=0.01)
        ),
        None,
    )
    if supported_scale is None:
        raise RuntimeError(
            f"native probe reported an unsupported display scale: {display_scale}"
        )
    display_scale = supported_scale
    scaled_width = normalized_native_bounds["width"] * display_scale
    scaled_height = normalized_native_bounds["height"] * display_scale
    minimum_width = max(1, math.floor(scaled_width))
    maximum_width = max(1, math.ceil(scaled_width))
    minimum_height = max(1, math.floor(scaled_height))
    maximum_height = max(1, math.ceil(scaled_height))
    if not (
        minimum_width <= png_width <= maximum_width
        and minimum_height <= png_height <= maximum_height
    ):
        raise RuntimeError(
            "native screenshot dimensions do not match the whole native window: "
            f"expected {minimum_width}..{maximum_width}x"
            f"{minimum_height}..{maximum_height}, "
            f"found {png_width}x{png_height}"
        )
    minimum_aspect = minimum_width / maximum_height
    maximum_aspect = maximum_width / minimum_height
    screenshot_aspect = png_width / png_height
    if not minimum_aspect <= screenshot_aspect <= maximum_aspect:
        raise RuntimeError(
            "native screenshot aspect ratio does not match the whole native "
            f"window: expected {minimum_aspect}..{maximum_aspect}, "
            f"found {screenshot_aspect}"
        )

    return {
        **probe,
        "display_scale": display_scale,
        "native_window_screenshot_aspect_max": maximum_aspect,
        "native_window_screenshot_aspect_min": minimum_aspect,
        "native_window_screenshot_bytes": len(png),
        "native_window_screenshot_height": png_height,
        "native_window_screenshot_width": png_width,
        "window_bounds": normalized_native_bounds,
    }


def validate_then_close(
    client: Any,
    *,
    browser_pid: int,
    expected_revision: str,
    screenshot_path: Path,
    native_probe: Callable[[float], dict[str, Any]],
    deadline: float,
    expected_url: str = DEFAULT_TAB_URL,
    poll_interval: float = 0.1,
) -> dict[str, Any]:
    validation_error: BaseException | None = None
    close_error: Exception | None = None
    result: dict[str, Any] | None = None
    try:
        result = _validate_browser_revision(
            client, expected_revision=expected_revision, deadline=deadline
        )
        target = _wait_for_single_real_page(
            client,
            expected_url=expected_url,
            deadline=deadline,
            poll_interval=poll_interval,
        )
        target_id = _require_string(target.get("targetId"), "real page target ID")
        window = client.call(
            "Browser.getWindowForTarget",
            {"targetId": target_id},
            deadline=deadline,
        )
        window_id = window.get("windowId")
        if (
            isinstance(window_id, bool)
            or not isinstance(window_id, int)
            or window_id <= 0
        ):
            raise RuntimeError(
                "Browser.getWindowForTarget returned an invalid window ID"
            )
        cdp_bounds = _validate_window_bounds(
            _require_mapping(window.get("bounds"), "CDP browser window bounds"),
            context="CDP browser window",
        )
        _remaining(deadline, "before starting the native boundary probe")
        native = _validate_native_probe(
            _require_mapping(native_probe(deadline), "native boundary probe result"),
            browser_pid=browser_pid,
            screenshot_path=screenshot_path,
            cdp_bounds=cdp_bounds,
        )
        _remaining(deadline, "revalidating browser bounds after native capture")
        window_after_capture = client.call(
            "Browser.getWindowForTarget",
            {"targetId": target_id},
            deadline=deadline,
        )
        if window_after_capture.get("windowId") != window_id:
            raise RuntimeError("CDP browser window identity changed during capture")
        cdp_bounds_after_capture = _validate_window_bounds(
            _require_mapping(
                window_after_capture.get("bounds"),
                "post-capture CDP browser window bounds",
            ),
            context="post-capture CDP browser window",
        )
        for key in ("left", "top", "width", "height"):
            if abs(cdp_bounds_after_capture[key] - cdp_bounds[key]) > BOUNDS_TOLERANCE:
                raise RuntimeError(f"CDP browser window {key} changed during capture")
        result.update(
            {
                "browser_pid": browser_pid,
                "cdp_window_bounds": cdp_bounds,
                "cdp_window_bounds_after_capture": cdp_bounds_after_capture,
                "cdp_window_id": window_id,
                "native_boundary": native,
                "page_target_count": 1,
                "target_id": target_id,
                "target_url": _require_string(target.get("url"), "real page URL"),
                "webui_browser_target_count": 0,
            }
        )
    except BaseException as exc:
        validation_error = exc

    try:
        client.send_command("Browser.close")
    except Exception as exc:
        close_error = exc

    if validation_error is not None:
        if close_error is not None:
            raise RuntimeError(
                f"Gate 2B validation failed: {validation_error}; "
                f"Browser.close request also failed: {close_error}"
            ) from validation_error
        raise validation_error
    if close_error is not None:
        raise RuntimeError(
            f"could not request Browser.close after Gate 2B validation: {close_error}"
        ) from close_error
    if result is None:
        raise RuntimeError("Gate 2B validation produced no result")
    result["browser_close_method"] = "Browser.close"
    result["browser_close_requested"] = True
    return result


def run_native_probe(
    probe: Path,
    *,
    browser_pid: int,
    screenshot_path: Path,
    deadline: float,
) -> dict[str, Any]:
    remaining = _remaining(deadline, "before starting the native boundary probe")
    probe_timeout = remaining - NATIVE_PROBE_CLEANUP_RESERVE
    if probe_timeout <= 0:
        raise TimeoutError(
            "insufficient time to start the native boundary probe with bounded cleanup"
        )
    command = [
        str(probe),
        "--pid",
        str(browser_pid),
        "--screenshot",
        str(screenshot_path),
        "--timeout",
        str(probe_timeout),
    ]
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=remaining,
        )
    except subprocess.TimeoutExpired as exc:
        raise TimeoutError("native boundary probe timed out") from exc
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or "no output"
        raise RuntimeError(
            f"native boundary probe failed with status {completed.returncode}: {detail}"
        )
    _remaining(deadline, "waiting for the native boundary probe")
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError("native boundary probe returned invalid JSON") from exc
    return _require_mapping(result, "native boundary probe JSON")


def validate_profile(
    profile: Path,
    *,
    browser_pid: int,
    expected_revision: str,
    screenshot_path: Path,
    native_probe_path: Path,
    timeout: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    port, websocket_path = cdp.read_debugger_endpoint(profile)
    with cdp.WebSocketConnection.connect(
        "127.0.0.1",
        port,
        websocket_path,
        _remaining(deadline, "connecting to the browser CDP endpoint"),
    ) as websocket:
        return validate_then_close(
            cdp.CdpClient(websocket),
            browser_pid=browser_pid,
            expected_revision=expected_revision,
            screenshot_path=screenshot_path,
            native_probe=lambda probe_deadline: run_native_probe(
                native_probe_path,
                browser_pid=browser_pid,
                screenshot_path=screenshot_path,
                deadline=probe_deadline,
            ),
            deadline=deadline,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--browser-pid", required=True, type=int)
    parser.add_argument("--expected-chromium-revision", required=True)
    parser.add_argument("--native-probe", required=True, type=Path)
    parser.add_argument("--screenshot", required=True, type=Path)
    parser.add_argument("--timeout", default=30.0, type=float)
    args = parser.parse_args()

    if args.browser_pid <= 0:
        parser.error("--browser-pid must be greater than zero")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if not cdp.REVISION_RE.fullmatch(args.expected_chromium_revision):
        parser.error(
            "--expected-chromium-revision must be a lowercase 40-character SHA"
        )
    if not args.native_probe.is_file():
        parser.error(f"--native-probe does not exist: {args.native_probe}")

    try:
        result = validate_profile(
            args.profile,
            browser_pid=args.browser_pid,
            expected_revision=args.expected_chromium_revision,
            screenshot_path=args.screenshot,
            native_probe_path=args.native_probe,
            timeout=args.timeout,
        )
    except (OSError, RuntimeError, TimeoutError) as exc:
        print(f"Gate 2B validation failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
