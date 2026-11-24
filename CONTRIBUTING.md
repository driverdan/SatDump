# Contributing to SatDump

See [tests/README.md](tests/README.md) for how to build and run the unit tests and where new tests go.

## Vendored libraries

SatDump carries copies of third-party code in its tree (Dear ImGui, OpenJPEG, libaec, nlohmann/json, ...). Each one is recorded in [`third_party.json`](third_party.json) at the repository root: where it lives, which upstream version and commit it came from, its license, whether SatDump has changed it, and how the build uses it.

**A pull request that changes a vendored path must update that library's entry in `third_party.json` in the same PR.** This covers updating to a new upstream version (change `version`, `commit` and `version_source`), patching the copy locally (update `local_changes`), adding a new vendored library (add an entry) and removing one (remove its entry). Formatting-only changes need no update.

When updating a library, record the exact upstream commit you copied from. If you only know the version, say so in `version_source` and leave `commit_verified` false.

Check the file before pushing:

```sh
python3 tools/check_third_party.py
```

It checks that the JSON parses without duplicate keys, that every entry has the fields below with valid values, that every listed path exists, that no path is listed twice or lies inside another listed path, that `satdump_files` are placed as described below, and that everything directly under `src-core/libs/` belongs to an entry.

CI (`.github/workflows/vendored_manifest.yml`) runs this on every pull request and push to master, and on pull requests also runs `tools/check_vendored_changes.py`. That fails if a file inside an entry's `paths` changed (added, modified, deleted or renamed) but the entry itself did not: the entry's JSON is compared between the base and the PR, so editing a different entry, moving the entry or reformatting the file does not count. Files in `satdump_files` are SatDump's own and need no update. Entries with `consumption` `system-first-fetchcontent-fallback` are not checked, since their path only names where the fetched version is set (for Catch2, `tests/CMakeLists.txt`). To run it locally against your branch:

```sh
python3 tools/check_vendored_changes.py --base origin/master
```

For a change that needs no manifest update (formatting, a build file), override the check with the `vendored-ok` label on the PR or a line containing only `[skip-vendor-check]` in the PR description. The check reruns when the label or description changes. The override does not apply to `tools/check_third_party.py`.

Paths must not nest: a file inside a directory that some entry lists would count as a change to both libraries, so when two libraries share a directory, list their files individually (as `imgui` and `implot` do for `src-core/imgui`). `satdump_files` are the exception: they may lie inside the entry's own `paths`, but not inside another entry's.

### Fields

The top level has `description`, `format_version` (currently `1`), `libraries` and `not_tracked`, a list of notes on third-party material that is deliberately not listed (git submodules, small fragments inside SatDump's own files). Each entry in `libraries` has these fields:

| Field | Meaning |
| --- | --- |
| `id` | Short unique name, e.g. `openjpeg`. |
| `name` | Human-readable name. |
| `paths` | Files and directories in the tree that hold the upstream code, relative to the repository root, with `/` and no trailing slash. |
| `satdump_files` | Optional. SatDump's own files that sit inside one of the entry's `paths` or in the same directory as one of them (e.g. `src-core/nlohmann/json_utils.cpp` next to `json.hpp`), which are not upstream code. A file may be claimed by only one entry, and must not also be in `paths`. |
| `upstream_url` | Upstream repository or home page, or `null` if unknown. |
| `version` | Upstream version the copy is based on, as precisely as known (e.g. `2.5.0`, `0.18 WIP`), or `unknown`. |
| `commit` | Full 40-character upstream commit SHA the copy is based on, `unknown`, or `n/a` when upstream has no commits to point at (tarball releases, closed-source headers). Never guess: use `unknown` until someone has checked. |
| `commit_verified` | `true` only if files were compared with upstream at `commit` and matched apart from the differences in `local_changes` (`version_source` says which files were compared, if not all). `false` if `commit` is only the tag of the recorded version. |
| `version_source` | Where `version` and `commit` come from (a version macro, a tag, a diff against upstream), and anything that contradicts them. |
| `license` | SPDX identifier(s) where possible, otherwise a short description. |
| `local_changes.status` | `yes` (SatDump has changed the upstream code), `no` (checked, unchanged), `unknown` (not yet checked), or `adapted` (heavily rewritten copy, merge upstream fixes by hand instead of re-applying patches). |
| `local_changes.details` | What was changed and where, or where the patches are kept (see issue #303). |
| `consumption` | How the build uses it: `vendored` (always built from the tree), `vendored-platform` (built from the tree on some platforms only, e.g. Windows shims), `system-first-vendored-fallback` (a system copy is used if found, the tree copy otherwise), or `system-first-fetchcontent-fallback` (a system copy is used if found, otherwise CMake FetchContent downloads it). |
| `upstream_state` | `newer-available`, `current`, `inactive` (upstream has had no changes for years), `archived`, `unknown`, or `n/a`. |
| `update_issue` | Number of the GitHub issue that tracks updating this library, or `null`. |
| `notes` | Anything else a maintainer updating the library should know. |
