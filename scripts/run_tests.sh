#!/usr/bin/env bash
# protoCore test runner with optional cache and rerun-failed support.
# Usage: run_tests.sh [--build-dir DIR] [--rerun-failed] [--no-cache] [--jobs N]
# Default: run all tests in parallel, save failed list to build/.protoCore_test_results.txt

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
RERUN_FAILED=false
NO_CACHE=false
JOBS=""

# Detect default parallel jobs
detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif [ -n "${NPROC}" ]; then
        echo "${NPROC}"
    else
        echo "4"
    fi
}

JOBS="$(detect_jobs)"

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --rerun-failed)
            RERUN_FAILED=true
            shift
            ;;
        --no-cache)
            NO_CACHE=true
            shift
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--build-dir DIR] [--rerun-failed] [--no-cache] [--jobs N]"
            echo "  --build-dir DIR   Use DIR as build directory (default: build)"
            echo "  --rerun-failed    Run only tests that failed in the last run (ctest -I Failed)"
            echo "  --no-cache        Do not read/write cache file (run full suite)"
            echo "  --jobs N          Number of parallel jobs (default: $(detect_jobs))"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

CACHE_FILE="${BUILD_DIR}/.protoCore_test_results.txt"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory does not exist: $BUILD_DIR" >&2
    echo "Configure and build first: cmake -B build -S . && cmake --build build --target proto_tests" >&2
    exit 1
fi

cd "$BUILD_DIR"

if [ "$RERUN_FAILED" = true ]; then
    if [ "$NO_CACHE" = false ] && [ -f "Testing/Temporary/LastTestsFailed.log" ]; then
        echo "Re-running only failed tests (from last CTest run)..."
        ctest -I Failed -j "$JOBS" --output-on-failure
    else
        echo "No failed tests cache found; running full test suite."
        ctest -j "$JOBS" --output-on-failure
    fi
    exit $?
fi

# Full run
if [ "$NO_CACHE" = true ]; then
    ctest -j "$JOBS" --output-on-failure
    exit $?
fi

# Run and save failed list for reference
ctest -j "$JOBS" --output-on-failure
EXIT_CODE=$?
if [ -f "Testing/Temporary/LastTestsFailed.log" ]; then
    mkdir -p "$(dirname "$CACHE_FILE")"
    cp "Testing/Temporary/LastTestsFailed.log" "$CACHE_FILE" 2>/dev/null || true
fi
exit $EXIT_CODE
