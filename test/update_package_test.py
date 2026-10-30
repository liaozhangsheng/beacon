#!/usr/bin/env python3
"""Checks the deterministic release package split used by CI."""

from __future__ import annotations

import json
from pathlib import Path
import stat
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import package_updates  # noqa: E402
from package_updates import build_updates  # noqa: E402

PREFIX = "beacon-1.2.3-linux/"
PROGRAM = "versions/1.2.3/"
BASE_URL = "https://example.test/releases/v1"


def make_archive(path: Path, extra: dict[str, bytes] | None = None, current: bytes = b"1.2.3\n") -> None:
    files = {
        "beacon": b"launcher",
        ".beacon-current": current,
        "LICENSE": b"project license",
        PROGRAM + "beacon": b"executable",
        PROGRAM + "libreal.so.1": b"real library",
        PROGRAM + "README.txt": b"any runtime file",
        PROGRAM + "plugins/codec.so": b"runtime subdirectory",
        PROGRAM + "SDL_image-LICENSE.txt": b"image license",
        PROGRAM + "assets/ui/icon.dat": b"asset",
        PROGRAM + "assets/fonts/LICENSE.txt": b"asset license",
        "templates/default/template.json": b"template",
        ".agents/skills/beacon-custom-template/SKILL.md": b"skill",
        "updater/beacon-updater": b"updater",
        **(extra or {}),
    }
    executables = {"beacon", PROGRAM + "beacon", "updater/beacon-updater"}
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(PREFIX, b"")
        archive.writestr(PREFIX + PROGRAM + "assets/", b"")
        for name, data in files.items():
            info = zipfile.ZipInfo(PREFIX + name)
            if name in executables:
                info.create_system = 3
                info.external_attr = 0o100755 << 16
            archive.writestr(info, data)
        link = zipfile.ZipInfo(PREFIX + PROGRAM + "libalias.so.0")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        archive.writestr(link, "libreal.so.1")


def members(path: Path) -> dict[str, bytes]:
    with zipfile.ZipFile(path) as archive:
        return {info.filename: archive.read(info) for info in archive.infolist() if not info.is_dir()}


class UpdatePackageTest(unittest.TestCase):
    def test_split_is_structural_deterministic_and_described_by_metadata(self) -> None:
        with tempfile.TemporaryDirectory(prefix="beacon-update-test-") as directory:
            root = Path(directory)
            archive = root / "beacon-1.2.3-Linux.zip"
            make_archive(archive)
            first = build_updates(archive, root / "first", "linux-x64", "1.2.3", BASE_URL)
            second = build_updates(archive, root / "second", "linux-x64", "1.2.3", BASE_URL)
            for kind in ("full", "runtime", "assets", "manifest"):
                self.assertEqual(first[kind].read_bytes(), second[kind].read_bytes(), kind)
            self.assertRegex(first["runtime"].name, r"^runtime-linux-x64-[0-9a-f]{64}\.zip$")
            self.assertRegex(first["assets"].name, r"^assets-[0-9a-f]{64}\.zip$")

            runtime = members(first["runtime"])
            self.assertEqual(
                set(runtime),
                {"beacon", "libreal.so.1", "libalias.so.0", "README.txt", "plugins/codec.so", "SDL_image-LICENSE.txt"},
            )
            self.assertEqual(runtime["libalias.so.0"], b"real library")
            with zipfile.ZipFile(first["runtime"]) as package:
                for info in package.infolist():
                    self.assertNotEqual((info.external_attr >> 16) & 0o170000, stat.S_IFLNK)
                self.assertTrue((package.getinfo("beacon").external_attr >> 16) & 0o111)
            self.assertEqual(set(members(first["assets"])), {"assets/ui/icon.dat", "assets/fonts/LICENSE.txt"})

            full = members(first["full"])
            for kept in ("beacon", ".beacon-current", "templates/default/template.json",
                         ".agents/skills/beacon-custom-template/SKILL.md", "updater/beacon-updater"):
                self.assertIn(PREFIX + kept, full)
            metadata = json.loads(full[PREFIX + PROGRAM + ".beacon-version.json"])
            manifest = json.loads(first["manifest"].read_bytes())
            self.assertEqual(set(manifest), {"version", "platform", "components"})
            self.assertEqual(set(metadata), {"version", "platform", "components"})
            self.assertEqual((metadata["version"], metadata["platform"]), ("1.2.3", "linux-x64"))
            for component in ("runtime", "assets"):
                info = manifest["components"][component]
                self.assertEqual(set(info), {"url", "sha256", "size"})
                self.assertEqual(info["size"], first[component].stat().st_size)
                self.assertEqual(info["url"], f"{BASE_URL}/{first[component].name}")
                self.assertEqual(metadata["components"][component], info["sha256"])

    def test_unchanged_assets_keep_their_identity_across_versions(self) -> None:
        with tempfile.TemporaryDirectory(prefix="beacon-update-test-") as directory:
            root = Path(directory)
            first_archive = root / "first.zip"
            second_archive = root / "second.zip"
            make_archive(first_archive)
            make_archive(second_archive, {PROGRAM + "beacon": b"new executable"})
            first = build_updates(first_archive, root / "first", "linux-x64", "1.2.3", BASE_URL)
            second = build_updates(second_archive, root / "second", "linux-x64", "1.2.3", BASE_URL)
            self.assertEqual(first["assets"].read_bytes(), second["assets"].read_bytes())
            self.assertNotEqual(first["runtime"].read_bytes(), second["runtime"].read_bytes())

    def test_rejects_invalid_layouts(self) -> None:
        cases = {
            "reserved-name": ({PROGRAM + "assets/CON.txt": b"bad"}, b"1.2.3\n"),
            "case-collision": ({PROGRAM + "assets/ui/ICON.dat": b"bad"}, b"1.2.3\n"),
            "file-parent": ({PROGRAM + "assets/ui/icon.dat/child": b"bad"}, b"1.2.3\n"),
            "reserved-metadata": ({PROGRAM + ".beacon-version.json": b"{}"}, b"1.2.3\n"),
            "other-version": ({"versions/1.2.2/beacon": b"old"}, b"1.2.3\n"),
            "pointer-mismatch": ({}, b"1.2.2\n"),
        }
        with tempfile.TemporaryDirectory(prefix="beacon-update-path-test-") as directory:
            root = Path(directory)
            for label, (extra, current) in cases.items():
                archive = root / f"{label}.zip"
                make_archive(archive, extra, current)
                with self.subTest(label=label), self.assertRaises(ValueError):
                    build_updates(archive, root / label, "linux-x64", "1.2.3", BASE_URL)
            archive = root / "valid.zip"
            make_archive(archive)
            for label, version, url in (("version", "1.2", BASE_URL), ("leading-zero", "01.2.3", BASE_URL),
                                        ("url", "1.2.3", "http://example.test/releases/v1")):
                with self.subTest(label=label), self.assertRaises(ValueError):
                    build_updates(archive, root / label, "linux-x64", version, url)

    def test_rejects_component_archives_over_client_limits(self) -> None:
        with tempfile.TemporaryDirectory(prefix="beacon-update-limit-test-") as directory:
            root = Path(directory)
            archive = root / "beacon-1.2.3-linux.zip"
            make_archive(archive)
            with patch.object(package_updates, "MAX_UNPACKED_BYTES", 1):
                with self.assertRaisesRegex(ValueError, "unpacked size limit"):
                    build_updates(archive, root / "oversized-unpacked", "linux-x64", "1.2.3", BASE_URL)
            with patch.object(package_updates, "MAX_PACKAGE_BYTES", 1):
                with self.assertRaisesRegex(ValueError, "package size limit"):
                    build_updates(archive, root / "oversized-archive", "linux-x64", "1.2.3", BASE_URL)
            with patch.object(package_updates, "MAX_ARCHIVE_FILES", 1):
                with self.assertRaisesRegex(ValueError, "too many files"):
                    build_updates(archive, root / "too-many-files", "linux-x64", "1.2.3", BASE_URL)


if __name__ == "__main__":
    unittest.main()
