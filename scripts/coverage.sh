#!/usr/bin/env bash
# Generate coverage report for protoCore using lcov and genhtml.
# Requires: build configured with -DCOVERAGE=ON, and lcov/genhtml installed.
# Usage: coverage.sh [BUILD_DIR]
#   BUILD_DIR defaults to "build" relative to project root.
# Output: BUILD_DIR/coverage/index.html

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-${PROJECT_ROOT}/build}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory does not exist: $BUILD_DIR" >&2
    exit 1
fi

cd "$BUILD_DIR"

if ! command -v lcov >/dev/null 2>&1; then
    echo "lcov is required for coverage reports. Install with: sudo apt-get install lcov (or equivalent)" >&2
    exit 1
fi

if ! command -v genhtml >/dev/null 2>&1; then
    echo "genhtml is required (part of lcov). Install lcov." >&2
    exit 1
fi

# Run tests to generate .gcda files
echo "Running tests..."
NPROC=$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)
ctest -j "${NPROC}" --output-on-failure || true

# Capture coverage
COVERAGE_INFO="${BUILD_DIR}/coverage.info"
COVERAGE_HTML="${BUILD_DIR}/coverage"

echo "Capturing coverage data..."
lcov --capture --directory . --output-file "$COVERAGE_INFO" --rc lcov_branch_coverage=0 2>/dev/null || \
    lcov --capture --directory . --output-file "$COVERAGE_INFO"

# Remove system/external paths
lcov --remove "$COVERAGE_INFO" \
    '/usr/*' \
    '*/_deps/*' \
    '*/googletest/*' \
    '*/build/*' \
    --output-file "$COVERAGE_INFO" 2>/dev/null || true

echo "Generating HTML report in ${COVERAGE_HTML}..."
genhtml "$COVERAGE_INFO" --output-directory "$COVERAGE_HTML" --no-branch-coverage 2>/dev/null || \
    genhtml "$COVERAGE_INFO" --output-directory "$COVERAGE_HTML"

echo "Coverage report: ${COVERAGE_HTML}/index.html"
