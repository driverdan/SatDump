# Unit tests

Automated unit tests for `satdump_core`, the `satdump` CLI (`src-cli`) and some of the plugins, built on [Catch2](https://github.com/catchorg/Catch2) v3 and run through CTest.

Catch2 3.4.0 or newer is required. A system install is used if it is recent enough, otherwise Catch2 3.16.1 is fetched and built at configure time (this needs network access on the first configure).

## Running

```sh
cmake -B build -DBUILD_UNIT_TESTS=ON
cmake --build build --target satdump_tests -j$(nproc)
ctest --test-dir build --output-on-failure
```

Add `-DCMAKE_BUILD_TYPE=Release` to test the optimized code that is actually shipped; the suite is expected to pass both with and without optimization.

The executable can also be run directly, which allows filtering by name or tag:

```sh
./build/satdump_tests "[repack]"
./build/satdump_tests "~[!shouldfail]"   # Skip the known bugs, which are noisy
./build/satdump_tests --list-tests
```

Catch2 runs the test cases in a random order when the executable is run directly, so tests must not depend on each other. A failure that only shows up in a full run can be replayed with the `--rng-seed` printed at the top of that run.

The tests are not built by default. The `Unit Tests` workflow (`.github/workflows/unit_tests.yml`) builds and runs them on Ubuntu 24.04, in Release and without the GUI, for every pull request and every push to `master`.

## Coverage

Coverage needs GCC or Clang, plus [gcovr](https://gcovr.com) for the report (`pip install gcovr` or your distribution's `gcovr` package). Use a separate build directory, as instrumented builds are unoptimized:

```sh
cmake -B build-coverage -DBUILD_UNIT_TESTS=ON -DENABLE_COVERAGE=ON
cmake --build build-coverage --target coverage -j$(nproc)
```

The `coverage` target runs the tests, prints a summary and writes an HTML report to `build-coverage/coverage/index.html` (and a Cobertura `coverage.xml` next to it). It covers `src-core`, excluding the vendored libraries, `src-cli`, and the plugins that have tests.

## Adding tests

Any `.cpp` file under this directory is picked up automatically. Tests mirror the layout of the source tree: the tests for `<root>/<path>` live in `tests/<root>/<path>`.

| Source | Test |
| --- | --- |
| `src-core/utils/string.h` | `tests/src-core/utils/test_string.cpp` |
| `src-cli/legacy/record.cpp` | `tests/src-cli/legacy/test_record.cpp` |
| `plugins/xrit_support/xrit/xrit_file.cpp` | `tests/plugins/xrit_support/xrit/test_xrit_file.cpp` |

Only what is shared by every source root sits directly in `tests/`: `main.cpp` and `helpers/`. To test another source root, create `tests/<root>/` and add it to `SATDUMP_TESTED_ROOTS` in `CMakeLists.txt`.

The sources of `src-cli` are compiled into the test executable, except `src-cli/main.cpp` (the entry point of `satdump`), which has no test. Where a source directory holds many small related files (most of `src-core/dsp`), one test file may cover the whole directory, e.g. `tests/src-core/dsp/pll/test_pll.cpp`.

**Plugins**

The tests of a plugin are only built when that plugin is, and the plugin is then linked into the test executable. A plugin with tests is declared in `CMakeLists.txt`, with its target and its directory under `plugins/`:

```cmake
satdump_add_plugin_tests(goes_support goes_support)
```

This also defines `SATDUMP_TEST_PLUGIN_<TARGET>`, the path of the built plugin: `plugins_test::loadTestPlugin()` loads it the way `satdump` does, so that it registers its modules or sources. Plugin tests are not built with MSVC, as plugins export no symbols there.

**Whole commands**

`tests/plugins/goes_support/test_goes_hrit_live.cpp` runs `satdump legacy live goes_hrit` from end to end against a rtl_tcp server made by the test, with the real plugins and the pipelines of `resources/`. It needs no hardware: SatDump only receives, so the transmit side is in the test helpers, and can be reused for other links.

| Helper | Provides |
| --- | --- |
| `helpers/ccsds_tx.h` | Reed-Solomon, randomization, NRZ-M and convolutional coding of CADUs, BPSK modulation, 8-bit I/Q |
| `plugins/xrit_support/xrit_test_helpers.h` | LRIT / HRIT files, their source packets (Rice compressed image lines included) and virtual channels |
| `plugins/xrit_support/xrit_decoder_test_helpers.h` | Runs the GK-2A, FengYun-4 and Elektro-L xRIT data decoders on such virtual channels, and lists what they wrote |
| `plugins/sdr_sources/rtltcp_support/rtltcp_test_helpers.h` | A rtl_tcp server that plays samples back and records the commands it is sent |

**ImGui callers**

Code that draws with Dear ImGui, ImPlot, ImPlot3D or imnodes is tested without a display through `imgui_test::HeadlessContext` (`src-core/imgui/imgui_test_helpers.h`). It creates the contexts, acts as a renderer backend that accepts the texture requests without a GPU, runs frames with the widgets drawn inside a window covering the display, and queues mouse and keyboard input (`click()`, `pressKey()`, `typeText()`...). Widgets are located from their item rectangles, e.g. `ImGui::GetItemRectMin()` right after drawing them.

## Conventions

Code is formatted with the repository's `.clang-format` (clang-format 19 or newer).

**Test cases**

- Every `TEST_CASE` has tags. For `src-core` the first tag is its top-level directory (`[common]`, `[dsp]`, `[image]`...), for `src-cli` it is `[cli]`, and for plugins it is `[plugins]` followed by the plugin (`[goes_support]`).
- Do not use an unbalanced `[` or `]` in a test name (e.g. `[0, 1)`), as the CTest discovery of older Catch2 versions then merges the following tests into one.
- Helpers local to one file go in an anonymous namespace, as every test file is linked into the same executable.
- Tests must pass in any order and leave no state behind. Global state (configuration, registries, working directory...) is changed through a guard that restores it, and files are only written to a `test_helpers::TempDir` / `TempFile`.
- Test data is deterministic: use `test_helpers::Lcg` rather than `rand()` or `std::random_device`.
- Blocking waits are bounded by a timeout, so a hang fails the test.
- Compare floating point values with the Catch2 matchers (`WithinAbs`, `WithinRel`) rather than a hand-written `fabs(a - b) < tol`, as they print both values on failure. `Catch::Approx` is still used by older tests.
- Do not assert once per sample over thousands of samples. Compute the worst case and assert on that.
- Do not compare large buffers with `==`, as both are printed in full on failure: `CHECK(test_helpers::bytesDiff(got, expected) == "")`.
- Use `M_PI`, like `src-core` does.

**Helpers**

- Helpers shared by the whole suite live in `tests/helpers/`, in the `test_helpers` namespace.
- Helpers for one area live next to its tests in `<area>_test_helpers.h`, in the `<area>_test` namespace (`dsp_test`, `ndsp_test`, `image_test`, `pipeline_test`...).
- Shared helpers are included relative to `tests/`: `#include "helpers/temp_files.h"`.
- Helpers for one area are included relative to `tests/<root>/`, like the headers of `<root>` are relative to `<root>/`: `#include "dsp/ndsp_test_helpers.h"`, `#include "cli_test_helpers.h"`. Their names must therefore be unique across source roots. Do not use `../`.

**Known bugs**

- A test that documents a bug in `src-core`, `src-cli` or a plugin is tagged `[!shouldfail]` and is preceded by a comment starting with `// Bug: <file>:<line>` that explains it. It starts failing (as "passed unexpectedly") once the bug is fixed; remove the tag then.
- If the bug only shows on some compilers, standard libraries or optimization levels, tag the test `[!mayfail]` instead and say so in the comment.
- Never exercise undefined behaviour or a crash from a test. Open a GitHub issue for such a problem instead, and refer to it by number in a comment where a test has to work around it.

**Portability**

- Code that needs POSIX headers (sockets, `pthread`...) is wrapped in `#ifndef _WIN32`.
- Tests that need a data symbol of `satdump_core` that is not declared in a header are wrapped in `#ifndef _MSC_VER`, as such symbols are not exported from the Windows DLL.

This is unrelated to `BUILD_TESTING` (the `src-testing` scratch program) and `tools/test_satdump` (batch testing against large recordings).
