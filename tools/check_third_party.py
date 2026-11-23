#!/usr/bin/env python3
"""Check third_party.json, the manifest of vendored libraries.

Checks that the file parses, that every entry has the documented fields with
valid values, that every listed path exists, that no path is listed twice or
nested inside another entry's path, and that every file or directory directly
under src-core/libs/ belongs to an entry.

Usage: python3 tools/check_third_party.py [path/to/third_party.json]
Exits 0 if the manifest is valid, 1 otherwise. Uses only the standard library.
See CONTRIBUTING.md for the meaning of the fields.
"""

import json
import os
import re
import sys

NoneType = type(None)

# field: (allowed types, must be non-empty if a string)
REQUIRED = {
    "id": (str, True),
    "name": (str, True),
    "paths": (list, False),
    "upstream_url": ((str, NoneType), True),
    "version": (str, True),
    "commit": (str, True),
    "commit_verified": (bool, False),
    "version_source": (str, True),
    "license": (str, True),
    "local_changes": (dict, False),
    "consumption": (str, True),
    "upstream_state": (str, True),
    "update_issue": ((int, NoneType), False),
    "notes": (str, False),
}
OPTIONAL = {"satdump_files": (list, False)}
TOP_LEVEL = {"description", "format_version", "libraries", "not_tracked"}

LOCAL_CHANGES_STATUS = {"yes", "no", "unknown", "adapted"}
CONSUMPTION = {
    "vendored",
    "vendored-platform",
    "system-first-vendored-fallback",
    "system-first-fetchcontent-fallback",
}
UPSTREAM_STATE = {"newer-available", "current", "inactive", "archived", "unknown", "n/a"}
SHA_RE = re.compile(r"^[0-9a-f]{40}$")

# Directories whose direct children must each belong to an entry
COVERED_DIRS = ["src-core/libs"]


def type_ok(value, kinds):
    if not isinstance(kinds, tuple):
        kinds = (kinds,)
    # bool is a subclass of int, but true/false is never a valid int field here
    if isinstance(value, bool) and bool not in kinds:
        return False
    return isinstance(value, kinds)


def path_error(p):
    """Return why p is not an acceptable manifest path, or None."""
    if not isinstance(p, str) or not p:
        return "must be a non-empty string"
    if p.startswith("/") or p.endswith("/") or "\\" in p or "//" in p:
        return "use a relative path with '/' separators and no trailing '/'"
    if any(part in (".", "..") for part in p.split("/")):
        return "must not contain '.' or '..' components"
    return None


def inside(p, q):
    """True if path p is q or lies under directory q."""
    return p == q or p.startswith(q + "/")


def check(data, root):
    errors = []
    if not isinstance(data, dict):
        return ["the top level must be a JSON object"]
    for key in data:
        if key not in TOP_LEVEL:
            errors.append(f"unknown top-level field '{key}'")
    if "not_tracked" in data and not (
        isinstance(data["not_tracked"], list) and all(isinstance(x, str) for x in data["not_tracked"])
    ):
        errors.append("'not_tracked' must be a list of strings")

    libraries = data.get("libraries")
    if not isinstance(libraries, list) or not libraries:
        return errors + ["'libraries' must be a non-empty list"]

    ids = set()
    owner = {}  # path -> id of the entry that lists it in 'paths'
    entries = []  # (where, id, valid paths, valid satdump_files)
    for n, lib in enumerate(libraries):
        if not isinstance(lib, dict):
            errors.append(f"entry {n}: not an object")
            continue
        lib_id = lib.get("id")
        where = f"entry {n} ({lib_id})" if isinstance(lib_id, str) else f"entry {n}"

        for key, (kinds, non_empty) in {**REQUIRED, **OPTIONAL}.items():
            if key not in lib:
                if key in REQUIRED:
                    errors.append(f"{where}: missing '{key}'")
                continue
            value = lib[key]
            if not type_ok(value, kinds):
                errors.append(f"{where}: '{key}' has the wrong type")
            elif non_empty and isinstance(value, str) and not value.strip():
                errors.append(f"{where}: '{key}' must not be empty")
        for key in lib:
            if key not in REQUIRED and key not in OPTIONAL:
                errors.append(f"{where}: unknown field '{key}'")

        if isinstance(lib_id, str):
            if lib_id in ids:
                errors.append(f"{where}: duplicate id")
            ids.add(lib_id)

        commit = lib.get("commit")
        if isinstance(commit, str) and commit and not (SHA_RE.match(commit) or commit in ("unknown", "n/a")):
            errors.append(f"{where}: 'commit' must be a full 40-character SHA, 'unknown' or 'n/a'")
        if lib.get("commit_verified") is True and not (isinstance(commit, str) and SHA_RE.match(commit)):
            errors.append(f"{where}: 'commit_verified' is true but 'commit' is not a SHA")
        if isinstance(lib.get("consumption"), str) and lib["consumption"] not in CONSUMPTION:
            errors.append(f"{where}: 'consumption' must be one of {sorted(CONSUMPTION)}")
        if isinstance(lib.get("upstream_state"), str) and lib["upstream_state"] not in UPSTREAM_STATE:
            errors.append(f"{where}: 'upstream_state' must be one of {sorted(UPSTREAM_STATE)}")
        changes = lib.get("local_changes")
        if isinstance(changes, dict):
            if changes.get("status") not in LOCAL_CHANGES_STATUS:
                errors.append(f"{where}: 'local_changes.status' must be one of {sorted(LOCAL_CHANGES_STATUS)}")
            if not isinstance(changes.get("details"), str):
                errors.append(f"{where}: 'local_changes.details' must be a string")
            for key in changes:
                if key not in ("status", "details"):
                    errors.append(f"{where}: unknown field 'local_changes.{key}'")

        good = {}
        for field in ("paths", "satdump_files"):
            values = lib.get(field)
            good[field] = []
            if not isinstance(values, list):
                continue
            if field == "paths" and not values:
                errors.append(f"{where}: 'paths' must not be empty")
            seen = set()
            for p in values:
                why = path_error(p)
                if why:
                    errors.append(f"{where}: bad entry {p!r} in '{field}': {why}")
                    continue
                if p in seen:
                    errors.append(f"{where}: {p} is listed twice in '{field}'")
                    continue
                seen.add(p)
                if not os.path.exists(os.path.join(root, p)):
                    errors.append(f"{where}: path does not exist: {p}")
                good[field].append(p)
        entries.append((where, lib_id, good["paths"], good["satdump_files"]))

        for p in good["paths"]:
            if p in owner:
                errors.append(f"{where}: {p} is also listed by {owner[p]}")
            else:
                owner[p] = lib_id

    # A path inside another entry's path would make a change count for both
    # libraries. satdump_files may sit inside paths: that is their purpose.
    for where, lib_id, paths, _ in entries:
        for p in paths:
            for q, other in owner.items():
                if q != p and inside(p, q):
                    if other == lib_id:
                        errors.append(f"{where}: {p} is inside {q}, which this entry already lists")
                    else:
                        errors.append(f"{where}: {p} is inside {q}, which belongs to {other}")

    listed = set(owner)
    for _, _, _, own in entries:
        listed.update(own)
    for d in COVERED_DIRS:
        full = os.path.join(root, d)
        if not os.path.isdir(full):
            continue
        for child in sorted(os.listdir(full)):
            rel = f"{d}/{child}"
            # Covered if listed itself, inside a listed directory, or if parts
            # of it are listed (a directory shared by vendored and own files)
            if not any(inside(rel, q) or q.startswith(rel + "/") for q in listed):
                errors.append(f"{rel} is not in any manifest entry")
    return errors


def main():
    if len(sys.argv) > 1:
        manifest = sys.argv[1]
    else:
        manifest = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "third_party.json")
    root = os.path.dirname(os.path.abspath(manifest))

    try:
        with open(manifest, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        print(f"{manifest}: {e}")
        return 1

    errors = check(data, root)
    for e in errors:
        print(f"{manifest}: {e}")
    if errors:
        return 1
    print(f"{manifest}: OK ({len(data['libraries'])} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
