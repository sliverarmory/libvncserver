#!/usr/bin/env python3
"""Record the exact source and installed binary hashes in CI artifacts."""
import hashlib
import json
import os
from pathlib import Path
import platform
import sys

root = Path(sys.argv[1]).resolve()
files = {
    str(path.relative_to(root)).replace(os.sep, "/"): hashlib.sha256(path.read_bytes()).hexdigest()
    for path in sorted(root.rglob("*"))
    if path.is_file() and path.name != "manifest.json"
}
if not any(name.endswith((".dll", ".dylib", ".so")) for name in files):
    raise SystemExit("No shared library installed")
(root / "manifest.json").write_text(json.dumps({
    "source_commit": os.environ.get("SOURCE_COMMIT", "local"),
    "platform": os.environ.get("ARTIFACT_PLATFORM", platform.platform()),
    "sha256": files,
}, indent=2) + "\n", encoding="utf-8")
