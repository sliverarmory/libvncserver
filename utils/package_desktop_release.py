#!/usr/bin/env python3
"""Validate tested desktop artifacts and package a complete GitHub release.

Only artifacts for the requested commit and all supported platforms are accepted.
The output directory must be absent or empty, so stale assets cannot be published.
"""

import argparse
from dataclasses import dataclass
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tarfile
import tempfile
import zipfile


PLATFORMS = {
    "linux-x86_64": "lib/libvncdesktop.so",
    "linux-arm64": "lib/libvncdesktop.so",
    "macos-x86_64": "lib/libvncdesktop.dylib",
    "macos-arm64": "lib/libvncdesktop.dylib",
    "windows-x86_64": "bin/vncdesktop.dll",
}
COMMON_FILES = {
    "include/rfb/desktop.h",
    "share/vncdesktop/COPYING",
    "share/vncdesktop/desktop-library.md",
}
TAG_PATTERN = re.compile(r"desktop-v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\."
                         r"(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?")
COMMIT_PATTERN = re.compile(r"[0-9a-f]{40}")
HASH_PATTERN = re.compile(r"[0-9a-f]{64}")


class ReleaseError(ValueError):
    """Input artifacts are incomplete, inconsistent, or unsafe to package."""


@dataclass
class Artifact:
    platform: str
    files: dict


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ReleaseError(f"Duplicate JSON key: {key!r}")
        value[key] = item
    return value


def safe_relative_name(name):
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        raise ReleaseError(f"Invalid manifest path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in name.split("/")):
        raise ReleaseError(f"Unsafe manifest path: {name!r}")
    if any(ord(character) < 32 or ord(character) == 127 for character in name):
        raise ReleaseError(f"Control character in manifest path: {name!r}")
    return name


def read_artifact(directory, platform, commit):
    if directory.is_symlink() or not directory.is_dir():
        raise ReleaseError(f"Artifact is not a regular directory: {directory}")
    files = {}
    for path in sorted(directory.rglob("*")):
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            raise ReleaseError(f"Symlinks are not permitted in artifacts: {path}")
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode):
            raise ReleaseError(f"Non-regular artifact file: {path}")
        name = safe_relative_name(path.relative_to(directory).as_posix())
        # Snapshot bytes once: both validation and archive generation use these.
        files[name] = path.read_bytes()

    try:
        manifest = json.loads(files["manifest.json"].decode("utf-8"),
                              object_pairs_hook=unique_object)
    except (KeyError, UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseError(f"Missing or invalid manifest in {directory.name}") from error
    if not isinstance(manifest, dict):
        raise ReleaseError(f"Manifest must be an object: {directory.name}")
    if manifest.get("source_commit") != commit:
        raise ReleaseError(f"Wrong source commit in {directory.name}")
    if manifest.get("platform") != platform:
        raise ReleaseError(f"Wrong platform in {directory.name}")
    hashes = manifest.get("sha256")
    if not isinstance(hashes, dict):
        raise ReleaseError(f"Missing hash map in {directory.name}")
    for name, digest in hashes.items():
        safe_relative_name(name)
        if not isinstance(digest, str) or not HASH_PATTERN.fullmatch(digest):
            raise ReleaseError(f"Invalid SHA-256 for {name!r} in {directory.name}")
    required = COMMON_FILES | {PLATFORMS[platform]}
    allowed = required | ({"lib/vncdesktop.lib"} if platform == "windows-x86_64" else set())
    if not required <= hashes.keys():
        raise ReleaseError(f"Missing required files in {directory.name}: "
                           f"{sorted(required - hashes.keys())}")
    if hashes.keys() - allowed:
        raise ReleaseError(f"Unexpected manifest files in {directory.name}: "
                           f"{sorted(hashes.keys() - allowed)}")
    if set(files) != set(hashes) | {"manifest.json"}:
        raise ReleaseError(f"Artifact file set differs from manifest: {directory.name}")
    for name, digest in hashes.items():
        if sha256(files[name]) != digest:
            raise ReleaseError(f"Checksum mismatch for {name!r} in {directory.name}")
    return Artifact(platform, files)


def validate_artifacts(root, commit):
    if root.is_symlink() or not root.is_dir():
        raise ReleaseError(f"Artifact root is not a regular directory: {root}")
    expected = {f"vncdesktop-{platform}-{commit}": platform for platform in PLATFORMS}
    actual = {path.name for path in root.iterdir()}
    if actual != expected.keys():
        raise ReleaseError("Expected exactly all five platform artifacts; "
                           f"missing={sorted(expected.keys() - actual)}, "
                           f"unexpected={sorted(actual - expected.keys())}")
    return [read_artifact(root / name, platform, commit)
            for name, platform in sorted(expected.items())]


def write_tar(path, prefix, artifact):
    with path.open("wb") as raw:
        # A fixed timestamp and empty gzip filename keep repeated packages equal.
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for name, data in sorted(artifact.files.items()):
                    info = tarfile.TarInfo(f"{prefix}/{name}")
                    info.size = len(data)
                    info.mode = 0o755 if name == PLATFORMS[artifact.platform] else 0o644
                    info.mtime = 0
                    archive.addfile(info, io.BytesIO(data))


def write_zip(path, prefix, artifact):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(artifact.files.items()):
            info = zipfile.ZipInfo(f"{prefix}/{name}", date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)


def package_release(artifacts_directory, output_directory, tag, commit):
    if not TAG_PATTERN.fullmatch(tag):
        raise ReleaseError("Tag must have the form desktop-vMAJOR.MINOR.PATCH[-PRERELEASE]")
    if not COMMIT_PATTERN.fullmatch(commit):
        raise ReleaseError("Commit must be a full lowercase 40-character Git SHA")
    artifacts_directory = Path(artifacts_directory)
    output_directory = Path(output_directory)
    if artifacts_directory.is_symlink() or output_directory.is_symlink():
        raise ReleaseError("Input and output directories may not be symlinks")
    artifacts_directory = artifacts_directory.resolve()
    output_directory = output_directory.resolve()
    if (artifacts_directory == output_directory or
            artifacts_directory in output_directory.parents or
            output_directory in artifacts_directory.parents):
        raise ReleaseError("Input and output directories must not overlap")
    if output_directory.exists() and (
            not output_directory.is_dir() or any(output_directory.iterdir())):
        raise ReleaseError("Output directory must be absent or empty")
    artifacts = validate_artifacts(artifacts_directory, commit)
    output_directory.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{output_directory.name}-",
                                     dir=output_directory.parent))
    try:
        assets = []
        for artifact in artifacts:
            prefix = f"vncdesktop-{tag[len('desktop-v'):]}-{artifact.platform}"
            extension = ".zip" if artifact.platform.startswith("windows-") else ".tar.gz"
            path = temporary / (prefix + extension)
            if extension == ".zip":
                write_zip(path, prefix, artifact)
            else:
                write_tar(path, prefix, artifact)
            assets.append({
                "name": path.name,
                "platform": artifact.platform,
                "size": path.stat().st_size,
                "sha256": sha256(path.read_bytes()),
            })
        metadata = {
            "schema_version": 1,
            "tag": tag,
            "source_commit": commit,
            "assets": assets,
        }
        (temporary / "release.json").write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        checksums = "".join(f"{sha256(path.read_bytes())}  {path.name}\n"
                            for path in sorted(temporary.iterdir()))
        (temporary / "SHA256SUMS").write_text(checksums, encoding="utf-8")
        if output_directory.exists():
            output_directory.rmdir()  # It was required to be empty; fail if changed.
        os.replace(temporary, output_directory)
        return metadata
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    try:
        metadata = package_release(args.artifacts, args.output, args.tag, args.commit)
    except (ReleaseError, OSError) as error:
        parser.exit(1, f"Release packaging failed: {error}\n")
    print(f"Validated and packaged {len(metadata['assets'])} platform archives for "
          f"{args.tag} ({args.commit}) in {args.output}")


if __name__ == "__main__":
    main()
