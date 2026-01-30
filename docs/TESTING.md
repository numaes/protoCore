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
