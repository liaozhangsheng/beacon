#!/usr/bin/env python3
"""Build deterministic update components from a CPack ZIP archive.

The archive holds the launcher, .beacon-current and one versions/<version>/
directory. That directory is split by structure: assets/ is the assets
component and everything else is the runtime component.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import posixpath
import re
import subprocess
import tempfile
from dataclasses import dataclass
from typing import Iterable
from urllib.parse import urlparse
import zipfile


ZIP_DATE = (1980, 1, 1, 0, 0, 0)
MAX_PACKAGE_BYTES = 512 * 1024 * 1024
MAX_UNPACKED_BYTES = 512 * 1024 * 1024
MAX_ARCHIVE_FILES = 100_000
MAX_TOKEN_LENGTH = 64
UINT64_MAX = (1 << 64) - 1
SYMLINK_MODE = 0o120000
CURRENT_POINTER = ".beacon-current"
VERSION_METADATA = ".beacon-version.json"
PLATFORM_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
VERSION_RE = re.compile(r"^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$")
RESERVED_WINDOWS_NAMES = {"con", "prn", "aux", "nul"}


@dataclass(frozen=True)
class Entry:
    name: str
    info: zipfile.ZipInfo
    data: bytes
    directory: bool


def _normal_name(name: str) -> str:
    """Return a safe, slash-separated archive member name."""
    name = name.replace("\\", "/")
    if "\x00" in name or name.startswith("/") or re.match(r"^[A-Za-z]:($|/)", name):
        raise ValueError(f"Unsafe ZIP member name: {name!r}")
    directory = name.endswith("/")
    parts = [part for part in name.split("/") if part not in ("", ".")]
    if any(part == ".." for part in parts):
        raise ValueError(f"Unsafe ZIP member name: {name!r}")
    normalized = "/".join(parts)
    return normalized + ("/" if directory and normalized else "")


def _is_symlink(info: zipfile.ZipInfo) -> bool:
    return ((info.external_attr >> 16) & 0o170000) == SYMLINK_MODE


def _is_executable(info: zipfile.ZipInfo, name: str) -> bool:
    mode = (info.external_attr >> 16) & 0o777
    return bool(mode & 0o111) or name.lower().endswith(".exe")


def _root_prefix(entries: Iterable[Entry]) -> str:
    names = [entry.name for entry in entries if entry.name.rstrip("/")]
    if not names:
        raise ValueError("The archive is empty")
    top_levels = {name.rstrip("/").split("/", 1)[0] for name in names}
    if len(top_levels) == 1:
        top = next(iter(top_levels))
        if any(name.startswith(top + "/") for name in names):
            return top + "/"
    return ""


def _read_entries(archive: Path) -> tuple[list[Entry], str]:
    with zipfile.ZipFile(archive) as source:
        raw: list[tuple[str, zipfile.ZipInfo, bytes, bool]] = []
        seen: set[str] = set()
        for info in source.infolist():
            name = _normal_name(info.filename)
            if not name or name in seen:
                if name in seen:
                    raise ValueError(f"Duplicate ZIP member name: {name}")
                continue
            seen.add(name)
            directory = info.is_dir() or name.endswith("/")
            raw.append((name, info, b"" if directory else source.read(info), directory))

    entries = [Entry(name, info, data, directory) for name, info, data, directory in raw]
    prefix = _root_prefix(entries)
    return entries, prefix


def _entry_data(entries: dict[str, Entry], name: str, active: set[str] | None = None) -> bytes:
    """Read an entry, resolving archive symlinks only within the archive root."""
    entry = entries[name]
    if entry.directory:
        return b""
    if not _is_symlink(entry.info):
        return entry.data

    active = set() if active is None else active
    if name in active:
        raise ValueError(f"Symlink cycle in ZIP member: {name}")
    active.add(name)
    target = entry.data.decode("utf-8", errors="strict").replace("\\", "/")
    if not target or target.startswith("/") or re.match(r"^[A-Za-z]:($|/)", target):
        raise ValueError(f"Invalid symlink target for ZIP member: {name}")
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
    if resolved == ".." or resolved.startswith("../"):
        raise ValueError(f"Symlink escapes archive root: {name} -> {target}")
    if resolved not in entries:
        raise ValueError(f"Symlink target is not in archive: {name} -> {target}")
    return _entry_data(entries, resolved, active)


def _relative_entries(entries: list[Entry], prefix: str) -> dict[str, Entry]:
    result: dict[str, Entry] = {}
    for entry in entries:
        if prefix and not entry.name.startswith(prefix):
            raise ValueError("The archive has members outside its package root")
        relative = entry.name[len(prefix) :] if prefix else entry.name
        if not relative:
            continue
        result[relative] = Entry(relative, entry.info, entry.data, entry.directory)
    return result


def _valid_path(name: str) -> bool:
    """Mirror of the updater's path rules: printable ASCII, portable names."""
    if not name or len(name) > 1024 or any(ord(char) < 0x21 or ord(char) > 0x7E for char in name):
        return False
    for part in name.split("/"):
        if not part or part in {".", ".."} or part.endswith((".", " ")):
            return False
        base = part.lower().split(".", 1)[0]
        if base in RESERVED_WINDOWS_NAMES or (len(base) == 4 and base[:3] in {"com", "lpt"} and base[3] in "123456789"):
            return False
        if any(char in part for char in '*?<>|":\\'):
            return False
    return True


def _component_of(path: str) -> str:
    """Components split a version directory by structure, as in the updater."""
    folded = path.casefold()
    return "assets" if folded == "assets" or folded.startswith("assets/") else "runtime"


def _program_members(relative: dict[str, Entry], version: str) -> dict[str, dict[str, Entry]]:
    """Split versions/<version>/ into runtime and assets members, keyed by path inside the version."""
    program_prefix = f"versions/{version}/"
    components: dict[str, dict[str, Entry]] = {"runtime": {}, "assets": {}}
    for name, entry in relative.items():
        if name.startswith("versions/") and name.rstrip("/") != "versions" and not name.startswith(program_prefix):
            raise ValueError(f"The full archive contains another program version: {name}")
        if not name.startswith(program_prefix) or name == program_prefix:
            continue
        path = name[len(program_prefix) :].rstrip("/")
        if path.casefold() == VERSION_METADATA:
            raise ValueError(f"{VERSION_METADATA} is reserved for the packaging script")
        component = _component_of(path)
        if component == "assets" and not entry.directory and path.casefold() == "assets":
            raise ValueError("assets must be a directory")
        components[component][path] = entry
    return components


def _validate_paths(members: dict[str, Entry], component: str) -> None:
    """Reject paths the updater would refuse: invalid names, case collisions, file parents."""
    folded: dict[str, bool] = {}
    for path, entry in members.items():
        key = path.casefold()
        if not _valid_path(path) or key in folded:
            raise ValueError(f"Invalid or duplicate {component} component path: {path}")
        folded[key] = entry.directory
    for path in members:
        parent = posixpath.dirname(path.casefold())
        while parent:
            if folded.get(parent) is False:
                raise ValueError(f"Conflicting {component} component paths: {path}")
            parent = posixpath.dirname(parent)


def _zip_info(name: str, executable: bool = False) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, ZIP_DATE)
    info.create_system = 3
    info.create_version = 20
    info.extract_version = 20
    info.flag_bits = 0x800
    if name.endswith("/"):
        info.external_attr = (0o40755 << 16) | 0x10
        info.compress_type = zipfile.ZIP_STORED
    else:
        info.external_attr = ((0o100755 if executable else 0o100644) << 16)
        info.compress_type = zipfile.ZIP_DEFLATED
    return info


def _write_zip(path: Path, members: dict[str, bytes], executable_names: set[str] | None = None) -> None:
    executable_names = executable_names or set()
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for name in sorted(members):
            output.writestr(_zip_info(name, name in executable_names), members[name])


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _url(base_url: str, filename: str) -> str:
    return f"{base_url.rstrip('/')}/{filename}" if base_url else filename


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, separators=(",", ": ")) + "\n").encode("utf-8")


def _sign(manifest: Path, signing_key: Path, signature: Path) -> None:
    signature.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(signing_key), "-in", str(manifest), "-out", str(signature)],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def build_updates(
    archive: Path,
    output: Path,
    platform: str,
    version: str,
    base_url: str,
    signing_key: Path | None = None,
) -> dict[str, Path]:
    if len(platform) > MAX_TOKEN_LENGTH or not PLATFORM_RE.fullmatch(platform):
        raise ValueError(f"Invalid platform: {platform!r}")
    if not VERSION_RE.fullmatch(version) or any(
        len(part) > 20 or int(part) > UINT64_MAX for part in version.split(".")
    ):
        raise ValueError(f"Version must be MAJOR.MINOR.PATCH: {version!r}")
    parsed_url = urlparse(base_url)
    if (
        parsed_url.scheme != "https"
        or not parsed_url.hostname
        or parsed_url.query
        or parsed_url.fragment
        or any(character.isspace() for character in base_url)
    ):
        raise ValueError("Base URL must be an HTTPS URL")
    try:
        parsed_url.port
    except ValueError as error:
        raise ValueError("Base URL must be an HTTPS URL") from error
    if not archive.is_file():
        raise FileNotFoundError(archive)

    output.mkdir(parents=True, exist_ok=True)
    source_entries, prefix = _read_entries(archive)
    relative = _relative_entries(source_entries, prefix)
    all_entries = {entry.name: entry for entry in relative.values()}

    pointer = relative.get(CURRENT_POINTER)
    if pointer is None or pointer.directory or _entry_data(all_entries, CURRENT_POINTER).strip() != version.encode():
        raise ValueError(f"{CURRENT_POINTER} must select version {version}")
    program_name = "beacon.exe" if platform.startswith("windows-") else "beacon"
    components = _program_members(relative, version)
    runtime_program = components["runtime"].get(program_name)
    if runtime_program is None or runtime_program.directory:
        raise ValueError(f"versions/{version}/{program_name} is missing")
    if not any(not entry.directory for entry in components["assets"].values()):
        raise ValueError("The full archive has no assets")

    component_paths: dict[str, Path] = {}
    component_info: dict[str, dict[str, object]] = {}
    program_prefix = f"versions/{version}/"
    for component in ("runtime", "assets"):
        _validate_paths(components[component], component)
        files = {path: entry for path, entry in components[component].items() if not entry.directory}
        if len(files) > MAX_ARCHIVE_FILES:
            raise ValueError(f"{component} component contains too many files")
        members = {path: _entry_data(all_entries, program_prefix + path) for path in files}
        if sum(map(len, members.values())) > MAX_UNPACKED_BYTES:
            raise ValueError(f"{component} component exceeds the unpacked size limit")
        executables = {path for path, entry in files.items() if path == program_name or _is_executable(entry.info, path)}
        temporary_fd, temporary_name = tempfile.mkstemp(prefix=f".{component}-", suffix=".zip", dir=output)
        os.close(temporary_fd)
        temporary = Path(temporary_name)
        try:
            _write_zip(temporary, members, executables)
            if temporary.stat().st_size > MAX_PACKAGE_BYTES:
                raise ValueError(f"{component} component exceeds the package size limit")
            digest = _sha256(temporary)
            filename = f"{component}-{platform}-{digest}.zip" if component == "runtime" else f"assets-{digest}.zip"
            destination = output / filename
            os.replace(temporary, destination)
        finally:
            temporary.unlink(missing_ok=True)
        component_paths[component] = destination
        component_info[component] = {
            "url": _url(base_url, filename),
            "sha256": digest,
            "size": destination.stat().st_size,
        }

    metadata = {
        "version": version,
        "platform": platform,
        "components": {component: component_info[component]["sha256"] for component in ("runtime", "assets")},
    }

    # Rebuild the full archive in a stable order while retaining every bundled
    # file (launcher, templates, updater, .agents) and add the version metadata.
    full_members: dict[str, bytes] = {}
    full_executables: set[str] = set()
    for name, entry in relative.items():
        full_name = prefix + name
        if entry.directory:
            full_members[full_name] = b""
        else:
            full_members[full_name] = _entry_data(all_entries, name)
            if _is_executable(entry.info, name):
                full_executables.add(full_name)
    full_members[prefix + program_prefix + VERSION_METADATA] = _json_bytes(metadata)

    full_path = output / archive.name
    temporary_full_fd, temporary_full_name = tempfile.mkstemp(prefix=".full-", suffix=".zip", dir=output)
    os.close(temporary_full_fd)
    temporary_full = Path(temporary_full_name)
    try:
        _write_zip(temporary_full, full_members, full_executables)
        os.replace(temporary_full, full_path)
    finally:
        temporary_full.unlink(missing_ok=True)

    manifest = {
        "version": version,
        "platform": platform,
        "components": {component: component_info[component] for component in ("runtime", "assets")},
    }
    manifest_path = output / f"release-{platform}.json"
    manifest_bytes = _json_bytes(manifest)
    if len(manifest_bytes) > 4 * 1024 * 1024:
        raise ValueError("Release manifest exceeds the updater's 4 MiB limit")
    if any(len(component["url"].encode("utf-8")) > 2_048 for component in component_info.values()):
        raise ValueError("Component URL exceeds the updater's 2048-character limit")
    manifest_path.write_bytes(manifest_bytes)
    signature_path = output / f"{manifest_path.name}.sig"
    if signing_key is not None:
        _sign(manifest_path, signing_key, signature_path)
    else:
        signature_path.unlink(missing_ok=True)

    return {
        "full": full_path,
        "runtime": component_paths["runtime"],
        "assets": component_paths["assets"],
        "manifest": manifest_path,
        **({"signature": signature_path} if signing_key is not None else {}),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True, help="CPack full ZIP")
    parser.add_argument("--output", type=Path, required=True, help="Output directory")
    parser.add_argument("--platform", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--signing-key", type=Path, help="Ed25519 PEM private key")
    args = parser.parse_args()
    try:
        outputs = build_updates(
            args.archive,
            args.output,
            args.platform,
            args.version,
            args.base_url,
            args.signing_key,
        )
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    for path in outputs.values():
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
