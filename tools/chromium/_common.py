#!/usr/bin/env python3
"""Shared, non-destructive Chromium workflow primitives for Limitless."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import shlex
import stat
import subprocess
import tempfile
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Mapping, Sequence


PIN_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
PATCH_NAME_RE = re.compile(r"^[0-9]{4}-[a-z0-9][a-z0-9-]*\.patch$")
PROFILE_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
SAFE_TARGET_RE = re.compile(r"^[A-Za-z0-9_./:+-]+$")

MANIFEST_KEYS = {"schema_version", "chromium_base_sha", "patches"}
PATCH_KEYS = {
    "file",
    "sha256",
    "upstream_files",
    "upstream_symbols",
    "limitless_targets",
    "tests",
    "conflict_signals",
    "removal_condition",
}


class ToolError(RuntimeError):
    """An actionable workflow error."""


@dataclass(frozen=True)
class Paths:
    limitless_root: Path
    workspace_root: Path
    chromium_src: Path
    pin_file: Path
    patches_dir: Path
    manifest_file: Path
    series_file: Path
    out_dir: Path
    args_file: Path
    branding_file: Path
    external_build_file: Path
    gn: Path
    autoninja: Path
    args_template: Path

    @classmethod
    def discover(cls, anchor: Path) -> "Paths":
        tool_dir = anchor.resolve().parent
        limitless_root = tool_dir.parents[1]
        workspace_root = limitless_root.parent
        chromium_src = workspace_root / "chromium" / "src"
        out_dir = chromium_src / "out" / "LimitlessDebug"
        return cls(
            limitless_root=limitless_root,
            workspace_root=workspace_root,
            chromium_src=chromium_src,
            pin_file=limitless_root / "chromium.version",
            patches_dir=limitless_root / "patches" / "chromium",
            manifest_file=limitless_root / "patches" / "chromium" / "manifest.json",
            series_file=limitless_root / "patches" / "chromium" / "series",
            out_dir=out_dir,
            args_file=out_dir / "args.gn",
            branding_file=limitless_root / "branding" / "BRANDING",
            external_build_file=limitless_root / "build" / "chromium" / "BUILD.gn",
            gn=chromium_src / "buildtools" / "mac" / "gn",
            autoninja=workspace_root / "depot_tools" / "autoninja",
            args_template=tool_dir / "args.limitless-debug.gn.in",
        )


@dataclass(frozen=True)
class PatchEntry:
    file: str
    sha256: str
    upstream_files: tuple[str, ...]
    upstream_symbols: tuple[str, ...]
    limitless_targets: tuple[str, ...]
    tests: tuple[str, ...]
    conflict_signals: tuple[str, ...]
    removal_condition: str


@dataclass(frozen=True)
class Manifest:
    chromium_base_sha: str
    patches: tuple[PatchEntry, ...]


@dataclass(frozen=True)
class Verification:
    pin: str
    chromium_head: str
    chromium_state: str
    limitless_status: str
    chromium_status: str
    manifest: Manifest


def ensure_supported_host() -> None:
    system = platform.system()
    machine = platform.machine()
    if system != "Darwin" or machine != "arm64":
        raise ToolError(
            "Limitless v1 Chromium tooling supports only macOS arm64. "
            f"Detected {system or 'unknown'} {machine or 'unknown'}."
        )


def require_file(path: Path, description: str, *, executable: bool = False) -> None:
    if not path.is_file():
        raise ToolError(f"Missing {description}: {path}")
    if executable and not os.access(path, os.X_OK):
        raise ToolError(f"{description.capitalize()} is not executable: {path}")


def command_text(argv: Sequence[os.PathLike[str] | str]) -> str:
    return shlex.join([os.fspath(arg) for arg in argv])


def run(
    argv: Sequence[os.PathLike[str] | str],
    *,
    cwd: Path,
    env: Mapping[str, str] | None = None,
    input_text: str | None = None,
    capture: bool = True,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    args = [os.fspath(arg) for arg in argv]
    try:
        completed = subprocess.run(
            args,
            cwd=cwd,
            env=dict(env) if env is not None else None,
            input=input_text,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None,
            check=False,
        )
    except OSError as exc:
        raise ToolError(f"Could not run {command_text(args)}: {exc}") from exc
    if check and completed.returncode != 0:
        details = "\n".join(
            part.strip()
            for part in (completed.stdout or "", completed.stderr or "")
            if part.strip()
        )
        suffix = f"\n{details}" if details else ""
        raise ToolError(
            f"Command failed with exit code {completed.returncode}: "
            f"{command_text(args)}{suffix}"
        )
    return completed


def git(
    paths: Paths, args: Sequence[str], **kwargs: object
) -> subprocess.CompletedProcess[str]:
    return run(["git", *args], cwd=paths.chromium_src, **kwargs)


def read_pin(paths: Paths) -> str:
    require_file(paths.pin_file, "Chromium pin file")
    raw = paths.pin_file.read_text(encoding="utf-8")
    lines = raw.splitlines()
    if len(lines) != 1 or not PIN_RE.fullmatch(lines[0]):
        raise ToolError(
            f"{paths.pin_file} must contain exactly one lowercase 40-character "
            "Chromium SHA."
        )
    return lines[0]


def chromium_head(paths: Paths) -> str:
    if not paths.chromium_src.is_dir():
        raise ToolError(f"Missing Chromium checkout: {paths.chromium_src}")
    result = git(paths, ["rev-parse", "--verify", "HEAD"])
    head = result.stdout.strip()
    if not PIN_RE.fullmatch(head):
        raise ToolError(f"Chromium returned an invalid HEAD SHA: {head!r}")
    return head


def _require_exact_keys(
    value: dict[str, object], expected: set[str], context: str
) -> None:
    actual = set(value)
    if actual == expected:
        return
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    details: list[str] = []
    if missing:
        details.append("missing " + ", ".join(missing))
    if extra:
        details.append("unexpected " + ", ".join(extra))
    raise ToolError(f"Invalid {context} keys: {'; '.join(details)}")


def _string_list(
    value: object,
    context: str,
    *,
    sorted_required: bool = False,
) -> tuple[str, ...]:
    if not isinstance(value, list) or not value:
        raise ToolError(f"{context} must be a non-empty JSON array.")
    if any(not isinstance(item, str) or not item.strip() for item in value):
        raise ToolError(f"Every item in {context} must be a non-empty string.")
    result = tuple(value)
    if len(set(result)) != len(result):
        raise ToolError(f"{context} contains duplicate values.")
    if sorted_required and list(result) != sorted(result):
        raise ToolError(f"{context} must be sorted lexicographically.")
    return result


def _safe_upstream_path(value: str, context: str) -> None:
    candidate = Path(value)
    if (
        candidate.is_absolute()
        or ".." in candidate.parts
        or value.startswith(("-", ":"))
    ):
        raise ToolError(f"{context} is not a safe Chromium-relative path: {value!r}")
    if value == "CHANGELOG.md" or value.endswith("/CHANGELOG.md"):
        raise ToolError(f"{context} may not name a CHANGELOG.md file.")


def _read_series(paths: Paths) -> tuple[str, ...]:
    require_file(paths.series_file, "Chromium patch series")
    raw = paths.series_file.read_text(encoding="utf-8")
    names: list[str] = []
    for line_number, line in enumerate(raw.splitlines(), start=1):
        name = line.strip()
        if not name:
            continue
        if line != name:
            raise ToolError(
                f"{paths.series_file}:{line_number} has leading or trailing whitespace."
            )
        if not PATCH_NAME_RE.fullmatch(name):
            raise ToolError(
                f"{paths.series_file}:{line_number} has an invalid patch name: {name!r}"
            )
        names.append(name)
    if len(set(names)) != len(names):
        raise ToolError(f"{paths.series_file} contains duplicate patch names.")
    return tuple(names)


def _patch_paths_from_numstat(paths: Paths, patch_path: Path) -> tuple[str, ...]:
    completed = subprocess.run(
        ["git", "apply", "--numstat", "-z", os.fspath(patch_path)],
        cwd=paths.chromium_src,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        details = completed.stderr.decode("utf-8", "replace").strip()
        suffix = f"\n{details}" if details else ""
        raise ToolError(f"Could not inspect patch paths in {patch_path}:{suffix}")

    tokens = completed.stdout.split(b"\0")
    if tokens and not tokens[-1]:
        tokens.pop()
    touched: set[str] = set()
    index = 0
    while index < len(tokens):
        fields = tokens[index].split(b"\t", 2)
        if len(fields) != 3:
            raise ToolError(f"Unexpected git apply --numstat output for {patch_path}")
        encoded_path = fields[2]
        if encoded_path:
            touched.add(encoded_path.decode("utf-8", "surrogateescape"))
            index += 1
            continue
        if index + 2 >= len(tokens):
            raise ToolError(f"Incomplete rename information in {patch_path}")
        touched.add(tokens[index + 1].decode("utf-8", "surrogateescape"))
        touched.add(tokens[index + 2].decode("utf-8", "surrogateescape"))
        index += 3
    return tuple(sorted(touched))


def load_manifest(paths: Paths, pin: str) -> Manifest:
    require_file(paths.manifest_file, "Chromium patch manifest")
    require_file(paths.series_file, "Chromium patch series")
    try:
        raw_manifest = json.loads(paths.manifest_file.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ToolError(f"Could not parse {paths.manifest_file}: {exc}") from exc
    if not isinstance(raw_manifest, dict):
        raise ToolError(f"{paths.manifest_file} must contain a JSON object.")
    _require_exact_keys(raw_manifest, MANIFEST_KEYS, "manifest")
    if raw_manifest["schema_version"] != 1:
        raise ToolError("manifest schema_version must be the integer 1.")
    base = raw_manifest["chromium_base_sha"]
    if not isinstance(base, str) or not PIN_RE.fullmatch(base):
        raise ToolError(
            "manifest chromium_base_sha must be a lowercase 40-character SHA."
        )
    if base != pin:
        raise ToolError(f"Manifest base {base} does not match chromium.version {pin}.")
    raw_patches = raw_manifest["patches"]
    if not isinstance(raw_patches, list):
        raise ToolError("manifest patches must be a JSON array.")

    entries: list[PatchEntry] = []
    for index, raw_entry in enumerate(raw_patches):
        context = f"manifest patch {index + 1}"
        if not isinstance(raw_entry, dict):
            raise ToolError(f"{context} must be a JSON object.")
        _require_exact_keys(raw_entry, PATCH_KEYS, context)
        filename = raw_entry["file"]
        digest = raw_entry["sha256"]
        removal = raw_entry["removal_condition"]
        if not isinstance(filename, str) or not PATCH_NAME_RE.fullmatch(filename):
            raise ToolError(f"{context} file has an invalid patch basename.")
        if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
            raise ToolError(f"{context} sha256 must be a lowercase 64-character hash.")
        if not isinstance(removal, str) or not removal.strip():
            raise ToolError(f"{context} removal_condition must be non-empty.")
        upstream_files = _string_list(
            raw_entry["upstream_files"],
            f"{context} upstream_files",
            sorted_required=True,
        )
        for upstream_file in upstream_files:
            _safe_upstream_path(upstream_file, f"{context} upstream_files")
        entry = PatchEntry(
            file=filename,
            sha256=digest,
            upstream_files=upstream_files,
            upstream_symbols=_string_list(
                raw_entry["upstream_symbols"], f"{context} upstream_symbols"
            ),
            limitless_targets=_string_list(
                raw_entry["limitless_targets"], f"{context} limitless_targets"
            ),
            tests=_string_list(raw_entry["tests"], f"{context} tests"),
            conflict_signals=_string_list(
                raw_entry["conflict_signals"], f"{context} conflict_signals"
            ),
            removal_condition=removal.strip(),
        )
        entries.append(entry)

    series = _read_series(paths)
    manifest_order = tuple(entry.file for entry in entries)
    if series != manifest_order:
        raise ToolError(
            "Patch order mismatch between series and manifest.\n"
            f"series:   {list(series)}\nmanifest: {list(manifest_order)}"
        )

    listed = set(series)
    present = {candidate.name for candidate in paths.patches_dir.glob("*.patch")}
    if present != listed:
        missing = sorted(listed - present)
        unlisted = sorted(present - listed)
        details: list[str] = []
        if missing:
            details.append("missing patches: " + ", ".join(missing))
        if unlisted:
            details.append("unlisted patches: " + ", ".join(unlisted))
        raise ToolError("Patch directory and series disagree: " + "; ".join(details))

    for entry in entries:
        patch_path = paths.patches_dir / entry.file
        actual_digest = hashlib.sha256(patch_path.read_bytes()).hexdigest()
        if actual_digest != entry.sha256:
            raise ToolError(
                f"Patch hash mismatch for {entry.file}: expected {entry.sha256}, "
                f"found {actual_digest}."
            )
        actual_paths = _patch_paths_from_numstat(paths, patch_path)
        if actual_paths != entry.upstream_files:
            raise ToolError(
                f"Touched files for {entry.file} do not match the manifest.\n"
                f"manifest: {list(entry.upstream_files)}\n"
                f"patch:    {list(actual_paths)}"
            )
    return Manifest(chromium_base_sha=base, patches=tuple(entries))


def _git_dir(paths: Paths) -> Path:
    result = git(paths, ["rev-parse", "--absolute-git-dir"])
    return Path(result.stdout.strip()).resolve()


def _strict_apply(
    paths: Paths,
    patch_path: Path,
    *,
    env: Mapping[str, str] | None = None,
    cached: bool = False,
    check_only: bool = False,
) -> None:
    args = ["git", "apply", "--verbose", "--whitespace=error-all"]
    if cached:
        args.append("--cached")
    if check_only:
        args.append("--check")
    args.extend(["--", os.fspath(patch_path)])
    completed = run(args, cwd=paths.chromium_src, env=env, check=False)
    output = "\n".join((completed.stdout or "", completed.stderr or ""))
    if re.search(r"\b(?:offset|fuzz)\b", output, flags=re.IGNORECASE):
        raise ToolError(
            f"Patch {patch_path.name} requires an offset or fuzz and was rejected.\n"
            f"{output.strip()}"
        )
    if completed.returncode != 0:
        details = output.strip()
        suffix = f"\n{details}" if details else ""
        raise ToolError(
            f"Patch {patch_path.name} failed strict application checking.{suffix}"
        )


@contextmanager
def temporary_git_index(
    paths: Paths,
) -> Iterator[tuple[dict[str, str], Path]]:
    """Yield an isolated Git index and object directory for patch operations."""
    git_dir = _git_dir(paths)
    object_directory = git_dir / "objects"
    with tempfile.TemporaryDirectory(prefix="limitless-patch-index-") as temporary:
        temporary_path = Path(temporary)
        scratch_objects = temporary_path / "objects"
        scratch_objects.mkdir()
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_INDEX_FILE": os.fspath(temporary_path / "index"),
                "GIT_OBJECT_DIRECTORY": os.fspath(scratch_objects),
                "GIT_ALTERNATE_OBJECT_DIRECTORIES": os.fspath(object_directory),
            }
        )
        yield environment, temporary_path


def populate_temporary_index(
    paths: Paths,
    entries: Sequence[PatchEntry],
    environment: Mapping[str, str],
) -> str:
    """Populate a temporary index with a strict patch prefix and return its tree."""
    run(
        ["git", "read-tree", "HEAD"],
        cwd=paths.chromium_src,
        env=environment,
    )
    for entry in entries:
        patch_path = paths.patches_dir / entry.file
        _strict_apply(
            paths,
            patch_path,
            env=environment,
            cached=True,
            check_only=True,
        )
        _strict_apply(paths, patch_path, env=environment, cached=True)
    return run(
        ["git", "write-tree"],
        cwd=paths.chromium_src,
        env=environment,
    ).stdout.strip()


def expected_patched_entries(
    paths: Paths, manifest: Manifest
) -> dict[str, tuple[str, str] | None]:
    """Return path -> (mode, blob SHA) for the exact fully patched tree."""
    if not manifest.patches:
        return {}
    with temporary_git_index(paths) as (environment, _):
        populate_temporary_index(paths, manifest.patches, environment)

        names_output = run(
            ["git", "diff", "--cached", "--name-only", "-z", "HEAD"],
            cwd=paths.chromium_src,
            env=environment,
        ).stdout
        names = [name for name in names_output.split("\0") if name]
        expected: dict[str, tuple[str, str] | None] = {}
        for name in names:
            stage = run(
                ["git", "ls-files", "--stage", "-z", "--", name],
                cwd=paths.chromium_src,
                env=environment,
            ).stdout
            if not stage:
                expected[name] = None
                continue
            record = stage.split("\0", 1)[0]
            metadata, recorded_name = record.split("\t", 1)
            mode, blob, stage_number = metadata.split(" ", 2)
            if recorded_name != name or stage_number != "0":
                raise ToolError(f"Unexpected temporary index state for {name}")
            expected[name] = (mode, blob)
        return expected


def _nul_names(completed: subprocess.CompletedProcess[str]) -> set[str]:
    return {name for name in completed.stdout.split("\0") if name}


def _actual_mode(path: Path) -> str:
    mode = path.lstat().st_mode
    if stat.S_ISLNK(mode):
        return "120000"
    if stat.S_ISREG(mode):
        return "100755" if mode & 0o111 else "100644"
    raise ToolError(f"Patched path has unsupported file type: {path}")


def _actual_blob(paths: Paths, relative_path: str) -> str:
    result = git(
        paths,
        ["hash-object", f"--path={relative_path}", "--", relative_path],
    )
    return result.stdout.strip()


def inspect_chromium_state(paths: Paths, manifest: Manifest) -> tuple[str, str]:
    status = git(paths, ["status", "--short", "--untracked-files=all"]).stdout.rstrip()
    staged = git(paths, ["diff", "--cached", "--quiet"], check=False)
    if staged.returncode not in (0, 1):
        raise ToolError("Could not inspect staged Chromium changes.")
    if staged.returncode == 1:
        raise ToolError(
            "Chromium has staged changes. Patch state must be entirely clean or "
            "the exact unstaged Limitless patch series.\n"
            f"{status or '(status unavailable)'}"
        )

    tracked = _nul_names(git(paths, ["diff", "--name-only", "-z", "HEAD"]))
    untracked = _nul_names(
        git(paths, ["ls-files", "--others", "--exclude-standard", "-z"])
    )
    actual_paths = tracked | untracked
    if not actual_paths:
        return "clean", status

    expected = expected_patched_entries(paths, manifest)
    if actual_paths != set(expected):
        unexpected = sorted(actual_paths - set(expected))
        missing = sorted(set(expected) - actual_paths)
        details: list[str] = []
        if unexpected:
            details.append("unexpected paths: " + ", ".join(unexpected))
        if missing:
            details.append("missing patched paths: " + ", ".join(missing))
        raise ToolError(
            "Chromium worktree is neither clean nor the exact recorded patch "
            f"series ({'; '.join(details)}).\n{status}"
        )

    mismatches: list[str] = []
    for relative_path, expected_entry in expected.items():
        absolute_path = paths.chromium_src / relative_path
        if expected_entry is None:
            if absolute_path.exists() or absolute_path.is_symlink():
                mismatches.append(f"{relative_path}: expected deletion")
            continue
        if not absolute_path.exists() and not absolute_path.is_symlink():
            mismatches.append(f"{relative_path}: missing")
            continue
        expected_mode, expected_blob = expected_entry
        actual_mode = _actual_mode(absolute_path)
        actual_blob = _actual_blob(paths, relative_path)
        if actual_mode != expected_mode or actual_blob != expected_blob:
            mismatches.append(
                f"{relative_path}: expected {expected_mode} {expected_blob}, "
                f"found {actual_mode} {actual_blob}"
            )
    if mismatches:
        raise ToolError(
            "Chromium changes touch recorded paths but do not exactly match the "
            "patch series:\n" + "\n".join(mismatches)
        )
    return "patched", status


def repository_status(repository: Path) -> str:
    if not repository.is_dir():
        raise ToolError(f"Missing repository: {repository}")
    result = run(["git", "status", "--short", "--untracked-files=all"], cwd=repository)
    return result.stdout.rstrip()


def verify(paths: Paths, *, require_state: str | None = None) -> Verification:
    ensure_supported_host()
    pin = read_pin(paths)
    head = chromium_head(paths)
    if head != pin:
        raise ToolError(
            f"Chromium HEAD does not match chromium.version.\n"
            f"expected: {pin}\nactual:   {head}\n"
            "Sync or select the recorded revision in a separate deliberate step. "
            "This tool will not modify the checkout."
        )
    manifest = load_manifest(paths, pin)
    state, chromium_status = inspect_chromium_state(paths, manifest)
    if require_state == "clean" and state != "clean":
        raise ToolError(
            "This operation requires a clean Chromium checkout, but the exact "
            "Limitless patch series is already applied."
        )
    if require_state == "patched" and state != "patched":
        if manifest.patches:
            raise ToolError(
                "This operation requires the recorded Limitless patch series. "
                "Run tools/chromium/apply-patches first."
            )
        raise ToolError("The manifest has no patches, so a patched state cannot exist.")
    limitless_status = repository_status(paths.limitless_root)
    return Verification(
        pin=pin,
        chromium_head=head,
        chromium_state=state,
        limitless_status=limitless_status,
        chromium_status=chromium_status,
        manifest=manifest,
    )


def required_integration_state(manifest: Manifest) -> str:
    return "patched" if manifest.patches else "clean"


def verify_integration(paths: Paths) -> Verification:
    initial = verify(paths)
    required = required_integration_state(initial.manifest)
    if initial.chromium_state != required:
        raise ToolError(
            "Chromium is clean but the manifest records patches. Run "
            "tools/chromium/apply-patches before generating, building, or running."
        )
    return initial


def gn_quote(value: Path | str) -> str:
    return json.dumps(os.fspath(value), ensure_ascii=True)


def rendered_args(paths: Paths) -> str:
    require_file(paths.gn, "pinned GN binary", executable=True)
    require_file(paths.args_template, "Limitless GN args template")
    template = paths.args_template.read_text(encoding="utf-8")
    replacements = {
        "@LIMITLESS_ROOT@": gn_quote(paths.limitless_root),
        "@BRANDING_FILE@": gn_quote(paths.branding_file),
    }
    for marker, value in replacements.items():
        count = template.count(marker)
        if count != 1:
            raise ToolError(
                f"GN args template must contain {marker} exactly once, found {count}."
            )
        template = template.replace(marker, value)
    formatted = run(
        [paths.gn, "format", "--stdin"],
        cwd=paths.chromium_src,
        input_text=template,
    ).stdout
    if not formatted.endswith("\n"):
        formatted += "\n"
    return formatted


def require_generation_inputs(paths: Paths) -> None:
    require_file(paths.branding_file, "Limitless BRANDING file")
    require_file(paths.external_build_file, "Limitless external GN build file")


def require_generated_args(paths: Paths) -> str:
    require_file(paths.args_file, "generated Limitless args.gn")
    expected = rendered_args(paths)
    actual = paths.args_file.read_text(encoding="utf-8")
    if actual != expected:
        raise ToolError(
            f"{paths.args_file} is stale or was edited manually. Run "
            "tools/chromium/gen to restore the deterministic configuration."
        )
    return actual


def validate_targets(targets: Iterable[str]) -> tuple[str, ...]:
    result = tuple(targets)
    if not result:
        raise ToolError("At least one Ninja target is required.")
    for target in result:
        if target.startswith("-") or not SAFE_TARGET_RE.fullmatch(target):
            raise ToolError(f"Unsafe or invalid Ninja target: {target!r}")
    return result


def parse_branding_product_name(paths: Paths) -> str:
    require_file(paths.branding_file, "Limitless BRANDING file")
    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(
        paths.branding_file.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ToolError(
                f"{paths.branding_file}:{line_number} is not a KEY=VALUE line."
            )
        key, value = line.split("=", 1)
        values[key] = value
    product_name = values.get("PRODUCT_FULLNAME", "")
    if not product_name or "/" in product_name or "\0" in product_name:
        raise ToolError(
            f"{paths.branding_file} must define a safe, non-empty PRODUCT_FULLNAME."
        )
    return product_name


def reject_managed_switches(arguments: Sequence[str]) -> None:
    managed = ("--user-data-dir", "--enable-features", "--disable-features")
    for argument in arguments:
        if any(argument == name or argument.startswith(name + "=") for name in managed):
            raise ToolError(
                f"{argument!r} is managed by Limitless tooling and may not be overridden."
            )


def safe_profile_name(value: str) -> str:
    if not PROFILE_NAME_RE.fullmatch(value):
        raise ToolError(
            "Profile name must start with an alphanumeric character and contain "
            "only alphanumerics, dot, underscore, or hyphen."
        )
    return value


def print_verification(verification: Verification, paths: Paths) -> None:
    print(f"Limitless root: {paths.limitless_root}")
    print(f"Chromium root:  {paths.chromium_src}")
    print(f"Pinned SHA:     {verification.pin}")
    print(f"Chromium HEAD:  {verification.chromium_head}")
    print(f"Patch count:    {len(verification.manifest.patches)}")
    print(f"Chromium state: {verification.chromium_state}")
    print("Limitless worktree:")
    print(verification.limitless_status or "  clean")
    print("Chromium worktree:")
    print(verification.chromium_status or "  clean")
