# protoCore documentation directory

This directory holds protoCore's documentation. The index of all documentation, including the documents in the repository root (README, DESIGN, CHANGELOG), is [DOCUMENTATION.md](../DOCUMENTATION.md).

## Contents

- [INSTALLATION.md](INSTALLATION.md) — Building from source, installing the shared library, and CPack packaging.
- [TESTING.md](TESTING.md) — Running the GoogleTest/CTest suite, re-running failures, parallel runs, coverage, and CI scripts.
- [MODULE_DISCOVERY.md](MODULE_DISCOVERY.md) — Specification of Unified Module Discovery (resolution chain, providers, module cache).
- [USER_GUIDE_UMD_MODULES.md](USER_GUIDE_UMD_MODULES.md) — Short guide to creating and registering a module for Unified Module Discovery.
- [GarbageCollector.md](GarbageCollector.md) — Garbage collector implementation and the phases of a collection cycle.
- [STW_ELIMINATION_RESEARCH.md](STW_ELIMINATION_RESEARCH.md) — Research note on bounding the stop-the-world pause.
- [MUTABLE_SHARDING_AND_CACHE_REFACTOR.md](MUTABLE_SHARDING_AND_CACHE_REFACTOR.md) — Dated design and results of the sharded mutable root and per-thread value cache (April 2026).
- [ROPES_AS_PROTOTUPLE.md](ROPES_AS_PROTOTUPLE.md) — Superseded note on strings as `ProtoTuple` ropes (February 2026); see DESIGN.md for the current string representation.
- [Structural description/](Structural%20description/README.md) — Guides (testing, creating modules) and architecture overviews (garbage collector, mutability model, object model).
- [archive/](archive/README.md) — Historical analyses and design specifications; not maintained.

## Generating the API reference

The API reference is generated from the C++ source comments with [Doxygen](https://www.doxygen.nl/), using the `Doxyfile` in the repository root. Install Doxygen with your package manager (for example `sudo apt-get install doxygen` on Debian/Ubuntu or `brew install doxygen` on macOS), then run from the repository root:

```bash
doxygen Doxyfile
```

The root `Doxyfile` scans the repository recursively (`INPUT = .`, `RECURSIVE = YES`), skipping paths that match its `EXCLUDE_PATTERNS` (including `*/docs/*` and `*/lib/*`), and writes XML only (`GENERATE_XML = YES`, `GENERATE_HTML = NO`, `GENERATE_LATEX = NO`) to `docs/doxygen/xml/`. The `docs/doxygen/` directory is ignored by git. Build directories inside the source tree are not excluded. `HAVE_DOT = YES` is set, so Graphviz's `dot` is used for diagrams when it is installed.

To produce HTML as well, override the setting on the command line:

```bash
( cat Doxyfile; echo "GENERATE_HTML = YES" ) | doxygen -
```

Then open `docs/doxygen/html/index.html` in a web browser.
