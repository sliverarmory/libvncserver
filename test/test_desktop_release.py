#!/usr/bin/env python3
"""Release packaging rejects incomplete or altered CI artifact sets."""

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest
import zipfile


SPEC = importlib.util.spec_from_file_location(
    "package_desktop_release",
    Path(__file__).resolve().parents[1] / "utils" / "package_desktop_release.py")
release = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = release
SPEC.loader.exec_module(release)

COMMIT = "a" * 40
TAG = "desktop-v0.1.0"


class DesktopReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        self.output = self.root / "release-assets"
        for platform, library in release.PLATFORMS.items():
            directory = self.directory(platform)
            directory.mkdir()
            files = release.COMMON_FILES | {library}
            if platform == "windows-x86_64":
                files |= {"lib/vncdesktop.lib"}
            hashes = {}
            for name in sorted(files):
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                contents = f"{platform}: {name}\n".encode()
                path.write_bytes(contents)
                hashes[name] = hashlib.sha256(contents).hexdigest()
            self.write_manifest(platform, {
                "source_commit": COMMIT, "platform": platform, "sha256": hashes,
            })

    def directory(self, platform="linux-x86_64"):
        return self.artifacts / f"vncdesktop-{platform}-{COMMIT}"

    def manifest(self, platform="linux-x86_64"):
        return json.loads((self.directory(platform) / "manifest.json").read_text())

    def write_manifest(self, platform, value):
        (self.directory(platform) / "manifest.json").write_text(json.dumps(value))

    def package(self, **options):
        return release.package_release(self.artifacts, self.output,
                                       options.get("tag", TAG), options.get("commit", COMMIT))

    def assert_rejected(self):
        with self.assertRaises(release.ReleaseError):
            self.package()
        self.assertFalse(self.output.exists(), "Invalid input left publishable output")

    def test_complete_release_has_verified_archives_and_checksums(self):
        metadata = self.package()
        self.assertEqual(metadata["source_commit"], COMMIT)
        self.assertEqual(metadata["tag"], TAG)
        self.assertEqual(len(metadata["assets"]), 5)
        self.assertEqual(len(list(self.output.iterdir())), 7)
        for asset in metadata["assets"]:
            path = self.output / asset["name"]
            self.assertEqual(asset["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(asset["size"], path.stat().st_size)
            prefix = f"vncdesktop-{TAG[len('desktop-v'):]}-{asset['platform']}/"
            expected = {prefix + name: data for name, data in
                        release.read_artifact(self.directory(asset["platform"]),
                                              asset["platform"], COMMIT).files.items()}
            if path.suffix == ".zip":
                with zipfile.ZipFile(path) as archive:
                    actual = {name: archive.read(name) for name in archive.namelist()}
            else:
                with tarfile.open(path) as archive:
                    actual = {member.name: archive.extractfile(member).read()
                              for member in archive.getmembers()}
            self.assertEqual(actual, expected)
        lines = (self.output / "SHA256SUMS").read_text().splitlines()
        self.assertEqual(len(lines), 6)
        for line in lines:
            digest, name = line.split("  ")
            self.assertEqual(digest, hashlib.sha256((self.output / name).read_bytes()).hexdigest())

    def test_archives_are_reproducible(self):
        self.package()
        another = self.root / "another-output"
        release.package_release(self.artifacts, another, TAG, COMMIT)
        self.assertEqual({p.name: p.read_bytes() for p in self.output.iterdir()},
                         {p.name: p.read_bytes() for p in another.iterdir()})

    def test_wrong_commit(self):
        manifest = self.manifest()
        manifest["source_commit"] = "b" * 40
        self.write_manifest("linux-x86_64", manifest)
        self.assert_rejected()

    def test_wrong_platform(self):
        manifest = self.manifest()
        manifest["platform"] = "linux-arm64"
        self.write_manifest("linux-x86_64", manifest)
        self.assert_rejected()

    def test_altered_binary(self):
        (self.directory() / "lib/libvncdesktop.so").write_bytes(b"changed after test")
        self.assert_rejected()

    def test_missing_platform(self):
        shutil.rmtree(self.directory("macos-arm64"))
        self.assert_rejected()

    def test_extra_artifact(self):
        (self.artifacts / "unexpected").mkdir()
        self.assert_rejected()

    def test_missing_required_file_even_when_manifest_is_updated(self):
        manifest = self.manifest()
        del manifest["sha256"]["include/rfb/desktop.h"]
        self.write_manifest("linux-x86_64", manifest)
        (self.directory() / "include/rfb/desktop.h").unlink()
        self.assert_rejected()

    def test_unlisted_file(self):
        (self.directory() / "foreign.bin").write_bytes(b"unlisted")
        self.assert_rejected()

    def test_foreign_file_cannot_be_authorized_by_manifest(self):
        contents = b"foreign file"
        (self.directory() / "foreign.bin").write_bytes(contents)
        manifest = self.manifest()
        manifest["sha256"]["foreign.bin"] = hashlib.sha256(contents).hexdigest()
        self.write_manifest("linux-x86_64", manifest)
        self.assert_rejected()

    def test_unsafe_manifest_paths(self):
        for name in ("../outside", "/absolute", "dir/../outside", "dir\\file", "C:/file", "dir//file"):
            with self.subTest(name=name):
                manifest = self.manifest()
                manifest["sha256"][name] = "0" * 64
                self.write_manifest("linux-x86_64", manifest)
                self.assert_rejected()
                del manifest["sha256"][name]
                self.write_manifest("linux-x86_64", manifest)

    def test_symlink(self):
        path = self.directory() / "lib/libvncdesktop.so"
        outside = self.root / "outside"
        outside.write_bytes(path.read_bytes())
        path.unlink()
        try:
            path.symlink_to(outside)
        except OSError as error:
            self.skipTest(f"Symlinks not available: {error}")
        self.assert_rejected()

    def test_duplicate_manifest_key(self):
        path = self.directory() / "manifest.json"
        contents = path.read_text()
        path.write_text('{"source_commit":"' + COMMIT + '",' + contents[1:])
        self.assert_rejected()

    def test_stale_output_is_not_overwritten(self):
        self.output.mkdir()
        sentinel = self.output / "old-release.zip"
        sentinel.write_bytes(b"preserve")
        with self.assertRaises(release.ReleaseError):
            self.package()
        self.assertEqual(sentinel.read_bytes(), b"preserve")

    def test_invalid_release_identifiers(self):
        for options in ({"tag": "v0.1.0"}, {"tag": "desktop-v1.0.0/../x"}, {"commit": "short"}):
            with self.subTest(options=options), self.assertRaises(release.ReleaseError):
                self.package(**options)


if __name__ == "__main__":
    unittest.main()
