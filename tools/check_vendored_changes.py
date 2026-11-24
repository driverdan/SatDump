#!/usr/bin/env python3
"""Check that a change to a vendored library updates its third_party.json entry.

Lists the files changed between two git revisions and maps each one to the
manifest entries whose 'paths' contain it ('satdump_files' are SatDump's own
code and do not count). Both the old and the new manifest are used, so adding
or removing a library is matched too. Every matched entry must differ between
the two revisions: the entry's parsed JSON is compared, so moving the entry or
reformatting the file does not count as a change, and touching another entry
does not either. Entries whose 'consumption' is
system-first-fetchcontent-fallback are skipped: their path only names where
the fetched version is set (e.g. tests/CMakeLists.txt), it holds no copy.

The check is skipped for a pull request that has the 'vendored-ok' label, or
a line '[skip-vendor-check]' in its description, read from the GitHub event
payload (--event, default $GITHUB_EVENT_PATH).

This does not check the manifest itself; tools/check_third_party.py does.

Usage: python3 tools/check_vendored_changes.py --base REV [--head REV] [--event FILE]
The diff is taken from the merge base of REV and the head (default HEAD), as
for 'git diff REV...HEAD'. Exits 0 if the check passes or is overridden, 1 if
it fails, 2 on a usage or git error. Uses only the standard library.
See CONTRIBUTING.md.
"""

import argparse
import json
import os
import subprocess
import sys

MANIFEST = "third_party.json"
OVERRIDE_LABEL = "vendored-ok"
OVERRIDE_LINE = "[skip-vendor-check]"
NOT_IN_TREE = {"system-first-fetchcontent-fallback"}


class GitError(Exception):
    pass


def git(root, *args):
    r = subprocess.run(["git", "-C", root, *args], capture_output=True)
    if r.returncode != 0:
        raise GitError(f"git {' '.join(args)}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout


def changed_files(root, base, head):
    # --no-renames: a rename counts as a deletion of the old path and an
    # addition of the new one, so both sides are checked
    out = git(root, "diff", "--name-only", "--no-renames", "-z", base, head, "--")
    return sorted(p for p in out.decode("utf-8", errors="surrogateescape").split("\0") if p)


def load_entries(root, rev):
    """Return {id: entry} of the manifest at rev ({} if the file is absent)."""
    try:
        text = git(root, "show", f"{rev}:{MANIFEST}")
    except GitError:
        # Absent at this revision (e.g. the PR that adds it); any other git
        # problem was already caught by the diff
        return {}
    try:
        data = json.loads(text)
    except ValueError as e:
        raise ValueError(f"{MANIFEST} at {rev} is not valid JSON: {e}")
    libraries = data.get("libraries") if isinstance(data, dict) else None
    entries = {}
    for lib in libraries if isinstance(libraries, list) else []:
        if isinstance(lib, dict) and isinstance(lib.get("id"), str):
            entries[lib["id"]] = lib
    return entries


def inside(p, q):
    """True if path p is q or lies under directory q."""
    return p == q or p.startswith(q + "/")


def str_list(value):
    return [p for p in value if isinstance(p, str) and p] if isinstance(value, list) else []


def owners(entries, path):
    """Ids of the entries whose vendored code includes path."""
    found = set()
    for lib_id, lib in entries.items():
        if lib.get("consumption") in NOT_IN_TREE:
            continue
        if any(inside(path, q) for q in str_list(lib.get("paths"))) and not any(
            inside(path, q) for q in str_list(lib.get("satdump_files"))
        ):
            found.add(lib_id)
    return found


def canonical(entry):
    return None if entry is None else json.dumps(entry, sort_keys=True, separators=(",", ":"))


def override(event_path):
    """Return the reason the check is overridden, or None."""
    if not event_path:
        return None
    try:
        with open(event_path, encoding="utf-8") as f:
            event = json.load(f)
    except (OSError, ValueError) as e:
        print(f"warning: cannot read the event payload {event_path}: {e}")
        return None
    pr = event.get("pull_request") if isinstance(event, dict) else None
    if not isinstance(pr, dict):
        return None
    labels = pr.get("labels") if isinstance(pr.get("labels"), list) else []
    if any(isinstance(l, dict) and l.get("name") == OVERRIDE_LABEL for l in labels):
        return f"the pull request has the '{OVERRIDE_LABEL}' label"
    body = pr.get("body") if isinstance(pr.get("body"), str) else ""
    if any(line.strip() == OVERRIDE_LINE for line in body.splitlines()):
        return f"the pull request description has a '{OVERRIDE_LINE}' line"
    return None


def annotate(message):
    """Print message, as an error annotation when run in GitHub Actions."""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        escaped = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::error title=Vendored library changed without a manifest update::{escaped}")
    else:
        print(f"error: {message}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--base", required=True, help="base revision (e.g. origin/master)")
    ap.add_argument("--head", default="HEAD", help="head revision (default HEAD)")
    ap.add_argument("--event", default=os.environ.get("GITHUB_EVENT_PATH"), help="GitHub event payload (JSON)")
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = ap.parse_args()

    try:
        merge_base = git(args.repo, "merge-base", args.base, args.head).decode().strip()
        files = changed_files(args.repo, merge_base, args.head)
        old = load_entries(args.repo, merge_base)
        new = load_entries(args.repo, args.head)
    except GitError as e:
        print(f"error: {e}")
        return 2
    except ValueError as e:
        # A broken manifest at the head is reported by check_third_party.py too
        print(f"error: {e}")
        return 1

    touched = {}  # id -> changed files of that library
    for f in files:
        for lib_id in owners(old, f) | owners(new, f):
            touched.setdefault(lib_id, []).append(f)

    stale = {i: fs for i, fs in touched.items() if canonical(old.get(i)) == canonical(new.get(i))}
    print(f"{len(files)} changed file(s), {len(touched)} vendored librar{'y' if len(touched) == 1 else 'ies'} touched")
    for lib_id in sorted(touched):
        state = "entry not updated" if lib_id in stale else "entry updated"
        print(f"  {lib_id}: {len(touched[lib_id])} file(s), {state}")
    if not stale:
        return 0

    reason = override(args.event)
    if reason:
        print(f"Check overridden because {reason}.")
        return 0
    for lib_id in sorted(stale):
        shown = stale[lib_id][:10]
        more = len(stale[lib_id]) - len(shown)
        annotate(
            f"Files of the vendored library '{lib_id}' changed but its entry in {MANIFEST} did not: "
            + ", ".join(shown)
            + (f" and {more} more" if more else "")
        )
    print(
        f"\nUpdate the entries above in {MANIFEST} (version, commit, local_changes, ...), "
        "see the 'Vendored libraries' section of CONTRIBUTING.md.\n"
        f"For a formatting-only or build-file change, add the '{OVERRIDE_LABEL}' label "
        f"or a line '{OVERRIDE_LINE}' to the pull request description."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
