#!/usr/bin/env python3
"""Check third_party.json, the manifest of vendored libraries.

Checks that the file parses, that every entry has the documented fields with
valid values, that every listed path exists, that no path is listed twice, and
that every file or directory directly under src-core/libs/ belongs to an entry.

Usage: python3 tools/check_third_party.py [path/to/third_party.json]
Exits 0 if the manifest is valid, 1 otherwise. Uses only the standard library.
See CONTRIBUTING.md for the meaning of the fields.
"""

import json
import os
import re
import sys

REQUIRED = {
    "id": str,
    "name": str,
    "paths": list,
    "upstream_url": (str, type(None)),
    "version": str,
    "commit": str,
    "commit_verified": bool,
    "version_source": str,
    "license": str,
    "local_changes": dict,
    "consumption": str,
    "upstream_state": str,
    "update_issue": (int, type(None)),
    "notes": str,
}
OPTIONAL = {"satdump_files": list}

LOCAL_CHANGES_STATUS = {"yes", "no", "unknown", "adapted"}
CONSUMPTION = {
    "vendored",
    "vendored-platform",
    "system-first-vendored-fallback",
    "system-first-fetchcontent-fallback",
}
UPSTREAM_STATE = {"newer-available", "current", "inactive", "archived", "unknown", "n/a"}
COMMIT_RE = re.compile(r"^([0-9a-f]{40}|unknown|n/a)$")

# Directories whose direct children must each belong to an entry
COVERED_DIRS = ["src-core/libs"]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    manifest = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "third_party.json")
    root = os.path.dirname(os.path.abspath(manifest))
    errors = []

    try:
        with open(manifest, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        print(f"{manifest}: {e}")
        return 1

    libraries = data.get("libraries")
    if not isinstance(libraries, list) or not libraries:
        print(f"{manifest}: 'libraries' must be a non-empty list")
        return 1

    ids = set()
    seen_paths = {}
    for n, lib in enumerate(libraries):
        where = f"entry {n} ({lib.get('id', '?')})" if isinstance(lib, dict) else f"entry {n}"
        if not isinstance(lib, dict):
            errors.append(f"{where}: not an object")
            continue

        for key, kind in REQUIRED.items():
            if key not in lib:
                errors.append(f"{where}: missing '{key}'")
            elif not isinstance(lib[key], kind) or (kind is int and isinstance(lib[key], bool)):
                errors.append(f"{where}: '{key}' has the wrong type")
        for key in lib:
            if key not in REQUIRED and key not in OPTIONAL:
                errors.append(f"{where}: unknown field '{key}'")
        for key, kind in OPTIONAL.items():
            if key in lib and not isinstance(lib[key], kind):
                errors.append(f"{where}: '{key}' has the wrong type")

        if lib.get("id") in ids:
            errors.append(f"{where}: duplicate id")
        ids.add(lib.get("id"))

        if isinstance(lib.get("commit"), str) and not COMMIT_RE.match(lib["commit"]):
            errors.append(f"{where}: 'commit' must be a full 40-character SHA, 'unknown' or 'n/a'")
        if lib.get("commit_verified") and not re.match(r"^[0-9a-f]{40}$", str(lib.get("commit"))):
            errors.append(f"{where}: 'commit_verified' is true but there is no commit")
        if lib.get("consumption") not in CONSUMPTION:
            errors.append(f"{where}: 'consumption' must be one of {sorted(CONSUMPTION)}")
        if lib.get("upstream_state") not in UPSTREAM_STATE:
            errors.append(f"{where}: 'upstream_state' must be one of {sorted(UPSTREAM_STATE)}")
        changes = lib.get("local_changes")
        if isinstance(changes, dict):
            if changes.get("status") not in LOCAL_CHANGES_STATUS:
                errors.append(f"{where}: 'local_changes.status' must be one of {sorted(LOCAL_CHANGES_STATUS)}")
            if not isinstance(changes.get("details"), str):
                errors.append(f"{where}: 'local_changes.details' must be a string")

        paths = lib.get("paths") if isinstance(lib.get("paths"), list) else []
        if not paths:
            errors.append(f"{where}: 'paths' must not be empty")
        for p in paths + list(lib.get("satdump_files") or []):
            if not isinstance(p, str) or p.startswith("/") or p.endswith("/") or "\\" in p:
                errors.append(f"{where}: bad path {p!r} (use relative paths with '/' and no trailing '/')")
                continue
            if not os.path.exists(os.path.join(root, p)):
                errors.append(f"{where}: path does not exist: {p}")
        for p in paths:
            if p in seen_paths and seen_paths[p] != lib.get("id"):
                errors.append(f"{where}: path {p} is also listed by {seen_paths[p]}")
            seen_paths.setdefault(p, lib.get("id"))

    listed = set()
    for lib in libraries:
        if isinstance(lib, dict):
            listed.update(lib.get("paths") or [])
            listed.update(lib.get("satdump_files") or [])

    def covered(p):
        return any(p == q or p.startswith(q + "/") for q in listed)

    for d in COVERED_DIRS:
        full = os.path.join(root, d)
        if not os.path.isdir(full):
            continue
        for child in sorted(os.listdir(full)):
            rel = f"{d}/{child}"
            if not covered(rel) and not any(q.startswith(rel + "/") for q in listed):
                errors.append(f"{rel} is not in any manifest entry")

    for e in errors:
        print(f"{manifest}: {e}")
    if errors:
        return 1
    print(f"{manifest}: OK ({len(libraries)} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
