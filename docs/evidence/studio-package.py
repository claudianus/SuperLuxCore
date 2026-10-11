#!/usr/bin/env python3
"""Validate the four-scene comparison publication set.

The former thirteen-scene showcase is archived. This check intentionally only
accepts the current eight-image, matched CPU technical preview and never
recreates or references the retired JPEG payload.
"""

import hashlib
import json
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # The hash/existence gate still works with system Python.
    Image = None

docs = Path(__file__).resolve().parents[1]
manifest_path = docs / "evidence" / "new-delta-manifest.json"
manifest = json.loads(manifest_path.read_text())
assert manifest["status"] == "technical-preview"
assert manifest["qualityGate"]["marketingReady"] is False
assert len(manifest["renders"]) == 8

for record in manifest["renders"]:
    full = docs.parent / record["full"]
    detail = docs.parent / record["detail"]
    assert full.is_file(), full
    assert detail.is_file(), detail
    assert hashlib.sha256(full.read_bytes()).hexdigest() == record["publishedImageSha256"], full
    if Image is not None:
        with Image.open(full) as image:
            assert list(image.size) == record["resolution"], full
        with Image.open(detail) as image:
            assert list(image.size) == record["detailResolution"], detail

print("Verified current new-delta technical preview:", len(manifest["renders"]), "render records")
