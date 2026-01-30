#!/usr/bin/env bash
# CI-friendly test runner: configure, build, run tests (optionally with coverage).
# Usage: ci_run_tests.sh [--build-dir DIR] [--jobs N] [--coverage]
# Exit code: 0 if all tests pass, non-zero otherwise.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
JOBS=""
COVERAGE=false

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
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --coverage)
            COVERAGE=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--build-dir DIR] [--jobs N] [--coverage]"
            echo "  --build-dir DIR   Use DIR as build directory (default: build)"
            echo "  --jobs N          Build and test parallel jobs (default: $(detect_jobs))"
            echo "  --coverage        Configure with -DCOVERAGE=ON and generate coverage report"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

cd "$PROJECT_ROOT"

CMAKE_OPTS=(-B "$BUILD_DIR" -S .)
if [ "$COVERAGE" = true ]; then
    CMAKE_OPTS+=(-DCOVERAGE=ON)
fi

echo "Configuring..."
cmake "${CMAKE_OPTS[@]}"

echo "Building protoCore and proto_tests..."
cmake --build "$BUILD_DIR" --target protoCore proto_tests -j "$JOBS"

echo "Running tests..."
ctest --test-dir "$BUILD_DIR" -j "$JOBS" --output-on-failure

if [ "$COVERAGE" = true ]; then
    echo "Generating coverage report..."
    if [ -x "$SCRIPT_DIR/coverage.sh" ]; then
        "$SCRIPT_DIR/coverage.sh" "$BUILD_DIR"
    else
        cmake --build "$BUILD_DIR" --target coverage
    fi
fi

echo "Done."
