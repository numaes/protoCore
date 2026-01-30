# User Guide: Running Tests and Coverage

This guide shows the minimal steps to run protoCore tests and generate a coverage report. All commands are intended to be run from the **project root** unless noted.

---

## 1. Build the project

From the project root:

```bash
cmake -B build -S .
cmake --build build --target protoCore proto_tests
```

---

## 2. Run all tests

**Recommended (parallel):**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

If your system does not have `nproc`, use a number instead, e.g.:

```bash
ctest --test-dir build -j4 --output-on-failure
```

**Using the helper script:**

```bash
./scripts/run_tests.sh
```

This runs tests in parallel and saves a list of failed tests for later re-runs.

---

## 3. Re-run only failed tests

If some tests failed and you fixed the code, you can re-run only the failed tests:

```bash
./scripts/run_tests.sh --rerun-failed
```

Or, from the build directory:

```bash
cd build
ctest -I Failed -j$(nproc) --output-on-failure
```

---

## 4. Generate and view coverage

**Step 1 – Build with coverage enabled:**

```bash
cmake -B build -S . -DCOVERAGE=ON
cmake --build build --target protoCore proto_tests
```

**Step 2 – Generate the report:**

```bash
cmake --build build --target coverage
```

Or run the script directly:

```bash
./scripts/coverage.sh build
```

**Step 3 – Open the report:**

Open in a browser:

- `build/coverage/index.html`

You need **lcov** and **genhtml** installed (e.g. `sudo apt-get install lcov` on Debian/Ubuntu).

---

## 5. One-command run (CI style)

To configure, build, and run tests in one go (e.g. for CI or a quick check):

```bash
./scripts/ci_run_tests.sh
```

With coverage:

```bash
./scripts/ci_run_tests.sh --coverage
```

---

## Quick reference

| Goal              | Command |
|-------------------|--------|
| Run tests         | `ctest --test-dir build -j$(nproc) --output-on-failure` |
| Run tests (script)| `./scripts/run_tests.sh` |
| Re-run failed     | `./scripts/run_tests.sh --rerun-failed` |
| Coverage report   | Build with `-DCOVERAGE=ON`, then `cmake --build build --target coverage` |
| Full CI run       | `./scripts/ci_run_tests.sh` |

For more detail (options, CI examples, filters), see the main [Testing Guide](../TESTING.md).
