# protoCore Testing Guide

This document describes how to run tests, use test caching, parallel execution, coverage analysis, and automated testing for protoCore.

## Overview

protoCore uses **Google Test** (GTest) for unit tests and **CTest** (CMake's test driver) for discovery and execution. Each test case is registered as a separate CTest test, so you can run the full suite or individual tests in parallel.

- **Test executable**: `build/test/proto_tests` (or `build_check/test/proto_tests` if you use the `build_check` directory).
- **Test source files**: All `*.cpp` files in the `test/` directory (e.g. `test_primitives.cpp`, `test_list.cpp`, `ContextTests.cpp`, etc.).

## Basic Execution

### Build and run all tests

From the project root:

```bash
cmake -B build -S .
cmake --build build --target protoCore proto_tests
ctest --test-dir build --output-on-failure
```

To use parallel jobs (recommended):

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

On systems without `nproc`, use a number (e.g. `-j4`).

### Run tests from the build directory

```bash
cd build
ctest -j$(nproc) --output-on-failure
```

## Test Caching and Re-running Failed Tests

### Using the run_tests script

The script `scripts/run_tests.sh` runs the test suite and optionally keeps a cache of failed tests for quick re-runs.

- **Default**: Run all tests in parallel and, after the run, save the list of failed tests to `build/.protoCore_test_results.txt` (for reference). CTest also keeps its own state in `build/Testing/Temporary/LastTestsFailed.log`.

- **Re-run only failed tests**: Use `--rerun-failed` to run only the tests that failed in the last run (uses CTest's "Failed" filter):

  ```bash
  ./scripts/run_tests.sh --rerun-failed
  ```

- **Ignore cache**: Use `--no-cache` to run the full suite without reading or writing the cache file:

  ```bash
  ./scripts/run_tests.sh --no-cache
  ```

- **Custom build dir or jobs**:

  ```bash
  ./scripts/run_tests.sh --build-dir build_check --jobs 8
  ```

### Using CTest directly

CTest supports re-running only failed tests from the last run:

```bash
cd build
ctest -I Failed -j$(nproc) --output-on-failure
```

This uses CTest's internal state (no script required).

## Parallel Execution

CTest runs each discovered test as a separate process. Use `-j N` to run up to `N` tests in parallel.

- **Recommended**: Use the number of CPU cores, e.g. `ctest -j$(nproc)` (Linux) or `ctest -j4` as a portable default.
- **CI**: Set `CTEST_PARALLEL_LEVEL` in the environment or in `CTestConfig.cmake` so that a bare `ctest` run uses parallelism. See the optional `CTestConfig.cmake` in the project root.

Example:

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

## Coverage Analysis

Coverage reports are generated with **gcov** and **lcov** (and **genhtml** for HTML output).

### Requirements

- **Compiler**: GCC or Clang (with gcov-style coverage).
- **Tools**: `lcov` and `genhtml` (usually provided by the `lcov` package, e.g. `apt-get install lcov` or equivalent).

### Build with coverage

Configure and build with coverage instrumentation:

```bash
cmake -B build -S . -DCOVERAGE=ON
cmake --build build --target protoCore proto_tests
```

### Generate the report

**Option 1 – Coverage target (when `COVERAGE=ON`):**

```bash
cmake --build build --target coverage
```

This runs the test suite and then generates the HTML report. The report is written to `build/coverage/index.html`.

**Option 2 – Script directly:**

```bash
./scripts/coverage.sh build
```

Open `build/coverage/index.html` in a browser to view line and function coverage for protoCore and the test binary.

### Interpreting the report

- **Line coverage**: Percentage of lines executed during tests.
- **Function coverage**: Percentage of functions entered.
- Focus on `core/` and project sources; exclude `_deps/` and system headers (the coverage script filters these where possible).

## Automated Testing and CI

### ci_run_tests script

For local or CI use, `scripts/ci_run_tests.sh` configures, builds, and runs the test suite in one go:

```bash
./scripts/ci_run_tests.sh
```

Options:

- `--build-dir DIR`: Use `DIR` as the build directory (default: `build`).
- `--jobs N`: Use `N` parallel jobs for build and tests (default: `nproc` or 4).
- `--coverage`: Configure with `-DCOVERAGE=ON` and generate the coverage report after tests.

Example with coverage:

```bash
./scripts/ci_run_tests.sh --coverage
```

Exit code: 0 if all tests pass, non-zero otherwise (suitable for CI).

### GitHub Actions (example)

Minimal workflow to build and run tests:

```yaml
name: Tests
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Configure and build
        run: |
          cmake -B build -S .
          cmake --build build --target protoCore proto_tests
      - name: Run tests
        run: ctest --test-dir build -j$(nproc) --output-on-failure
```

Or use the CI script:

```yaml
      - name: Run tests
        run: chmod +x scripts/ci_run_tests.sh && ./scripts/ci_run_tests.sh
```

### GitLab CI (example)

```yaml
test:
  script:
    - cmake -B build -S .
    - cmake --build build --target protoCore proto_tests
    - ctest --test-dir build -j$(nproc) --output-on-failure
```

Or:

```yaml
test:
  script:
    - chmod +x scripts/ci_run_tests.sh
    - ./scripts/ci_run_tests.sh
```

To add coverage and publish artifacts, configure with `-DCOVERAGE=ON`, run tests, run `scripts/coverage.sh`, and upload the `build/coverage/` directory as an artifact.

## Running a Subset of Tests

### By test name (GTest filter)

Run only tests whose name matches a pattern:

```bash
./build/test/proto_tests --gtest_filter='ListTest.*'
./build/test/proto_tests --gtest_filter='*Creation*'
```

### By CTest regex

Run CTest tests matching a regular expression:

```bash
ctest --test-dir build -R 'PrimitivesTest' -j$(nproc) --output-on-failure
```

## Summary

| Task                 | Command or script |
|----------------------|-------------------|
| Run all tests        | `ctest --test-dir build -j$(nproc) --output-on-failure` or `./scripts/run_tests.sh` |
| Re-run failed tests  | `./scripts/run_tests.sh --rerun-failed` or `ctest -I Failed -j$(nproc) --output-on-failure` (from `build/`) |
| Coverage report      | Configure with `-DCOVERAGE=ON`, then `cmake --build build --target coverage` or `./scripts/coverage.sh build` |
| Full CI run          | `./scripts/ci_run_tests.sh` (optionally with `--coverage`) |

For a short, copy-paste oriented guide, see [Testing User Guide](Structural%20description/guides/04_testing_user_guide.md).


## Conformance — the embedder suite

`docs/EMBEDDER-CONFORMANCE.md` is the normative rule table; this section is how
it runs.

protoCore's participation obligations are executable. Thirteen cases live in
`libprotoCoreConformance` and are driven through a `proto::conformance::Host`
adaptor that each embedder implements itself, so protoCore states each obligation
once and every runtime executes the same statement. The library is framework-free
— cases return results as data and the embedder's own framework asserts — and it
never names a runtime; both properties are asserted by
`test/ConformanceSelfCheckTests.cpp` rather than intended.

```bash
# protoCore's own reference run, against its SelfHost
ctest --test-dir build_release -R 'ConformanceSelfCheck|conformance.isolate' < /dev/null

# one case, in its own process
build_release/conformance/protocore-conformance-isolate --list
build_release/conformance/protocore-conformance-isolate --case=gc.young_submitted

# the static half, over any embedder tree
python3 scripts/conformance/check_static.py --repo ../protoPython
python3 scripts/conformance/check_static.py --self-test
```

One sweep worth knowing about, because it needs no adaptor at all. Rule 13 — *a
cycle among mutable objects is never collected* — is asked of a whole program by
an environment variable, and the file form of it does not disturb a single test
that diffs stderr:

```bash
PROTOCORE_MUTABLE_CYCLE_CHECK=/tmp/scan.txt ctest --test-dir build_release < /dev/null
grep -c '  CYCLE ' /tmp/scan.txt        # empty file = no space was destroyed, NOT a clean graph
```

The property itself is measured in `test/MutableCycleDetectorTests.cpp`, whose
mutation matrix is one line wide: the same builder either captures a mutable
handle (the detector must fire, naming both closing attributes) or the handle's
current value (it must be silent, while still counting the reference, so its
silence is not the silence of a scan that saw nothing).

Three things to know before reading a result.

**`NotApplicable` is never `Pass`.** It means a capability the case needs is not
implemented, so **the rule is unverified for that runtime**. A green board with
`NotApplicable` rows is not a clean board.

**Three cases must run in their own process**, because their failure destroys the
run instead of reporting: `heap.ceiling_progress` fails by `std::abort()` inside
`waitForHeapHeadroom`, and `join.parks` and `stw.quorum_completes` fail by
deadlocking the whole space — a thread that cooperates with stop-the-world parks
inside `safepoint()` waiting for a flag that a never-starting collection will
never clear, so no bound inside the case can rescue it. They are ctest entries
with a `TIMEOUT`, and **for the two deadlocking cases the timeout is the
verdict**.

**Every `ctest` invocation takes `< /dev/null`.** A runner that hangs is
indistinguishable from a conformance failure that hangs, and those are exactly
the rules whose failure mode is a hang.

Writing a new GC test in protoCore itself: do not assert `reclaimed > 0` or
`freeAfter > freeBefore` over a bulk workload. Use
`conformance/CycleDriver.h::checkProportionalReclaim`, which takes the
denominator as an argument and offers no way to ask the vacuous question.
