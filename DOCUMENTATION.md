# protoCore Documentation Index

Index of the documentation in this repository. Documents marked **dated** record the state of protoCore when they were written and may not match the current code.

---

## 1. User documentation

| Document | Contents |
|----------|----------|
| [README.md](README.md) | Project overview, status, ecosystem, quick start, and build, test and packaging instructions. |
| [docs/INSTALLATION.md](docs/INSTALLATION.md) | Building from source, installing the shared library, and CPack packaging per platform. |
| [docs/TESTING.md](docs/TESTING.md) | Test suite (GoogleTest and CTest): running tests, re-running failures, parallel runs, coverage, and CI scripts. |
| [docs/Structural description/guides/04_testing_user_guide.md](docs/Structural%20description/guides/04_testing_user_guide.md) | Short copy-paste guide to running the tests and generating a coverage report. |
| [docs/USER_GUIDE_UMD_MODULES.md](docs/USER_GUIDE_UMD_MODULES.md) | Short guide to creating and registering a module for Unified Module Discovery (UMD). |
| [docs/MODULE_DISCOVERY.md](docs/MODULE_DISCOVERY.md) | UMD specification: `ProviderRegistry`, `ModuleProvider`, resolution chain, `ProtoSpace::getImportModule`, `SharedModuleCache`, `FileSystemProvider`, platform defaults. |
| [docs/Structural description/guides/05_creating_modules.md](docs/Structural%20description/guides/05_creating_modules.md) | Step-by-step guide to implementing, registering and loading a custom `ModuleProvider`, with an example. |
| [LICENSE](LICENSE) | License terms. |

## 2. Architecture overviews

| Document | Contents |
|----------|----------|
| [docs/Structural description/README.md](docs/Structural%20description/README.md) | Introduction to protoCore and index of the guides and architecture overviews below. |
| [docs/Structural description/architecture/01_garbage_collector.md](docs/Structural%20description/architecture/01_garbage_collector.md) | Garbage collector overview: design without write barriers, collection life cycle, critical sections, external buffers. |
| [docs/Structural description/architecture/02_mutability_model.md](docs/Structural%20description/architecture/02_mutability_model.md) | Mutability model overview: identity/state separation, sharded `mutableRoot`, compare-and-swap updates, GC root scanning. |
| [docs/Structural description/architecture/03_object_model.md](docs/Structural%20description/architecture/03_object_model.md) | Object model overview: context life cycle, tagged pointers, prototype-based inheritance, thread-local attribute cache. |

## 3. Contributor and design documentation

| Document | Contents |
|----------|----------|
| [DESIGN.md](DESIGN.md) | Architectural design and implementation rules: public API versus internal classes, memory model, garbage collector, unmanaged regions, heap allocation limit, data model, object model, two-tier cache, execution model. |
| [CHANGELOG.md](CHANGELOG.md) | Release notes. |
| [docs/GarbageCollector.md](docs/GarbageCollector.md) | Garbage collector implementation: `ProtoSpace`, `ProtoContext`, `DirtySegment`, the mutable-shard snapshot, and each phase of a collection cycle. |
| [docs/STW_ELIMINATION_RESEARCH.md](docs/STW_ELIMINATION_RESEARCH.md) | Research note on bounding the stop-the-world pause. Its concurrent-mark step is implemented (2026-05-30); the other directions are research only. |
| [docs/MUTABLE_SHARDING_AND_CACHE_REFACTOR.md](docs/MUTABLE_SHARDING_AND_CACHE_REFACTOR.md) | **Dated** (April 2026): design and measured results of the 256-shard mutable root and the per-thread mutable value cache. |
| [docs/ROPES_AS_PROTOTUPLE.md](docs/ROPES_AS_PROTOTUPLE.md) | **Dated** (February 2026): `ProtoString` as `ProtoTuple` ropes. It predates the three-tier string redesign ([design specification](docs/archive/design-specs/2026-03-31-string-refactoring-design.md)) and may not match the current representation. |

## 4. Dated analyses

| Document | Contents |
|----------|----------|
| [TECHNICAL_ANALYSIS.md](TECHNICAL_ANALYSIS.md) | **Dated** (January 2026, updated April 2026): high-level overview of the architecture, codebase layout, build system and technology stack. |
| [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) | **Dated** (January 2026): record of the methods added to protoCore for protoJS's `GCBridge` (`ProtoString::asObject` and others). |
| [JIT_IMPACT_ANALYSIS_2026.md](JIT_IMPACT_ANALYSIS_2026.md) | **Dated** (April 2026): analysis of a built-in JIT; recommends optimization hooks in protoCore instead of a self-contained JIT. |
| [RUNTIME_STRATEGY_2026.md](RUNTIME_STRATEGY_2026.md) | **Dated** (April 2026): comparison with BEAM, Pony, Clojure and Lua/QuickJS, suggested application domains, and proposed next steps. |

## 5. API reference

The API reference is generated with Doxygen from the root [Doxyfile](Doxyfile): `doxygen Doxyfile` writes XML to `docs/doxygen/xml/`, which is not tracked. [docs/README.md](docs/README.md) describes the configuration and how to generate HTML.

## 6. Archive

| Document | Contents |
|----------|----------|
| [docs/archive/README.md](docs/archive/README.md) | Historical audits, plans and one-off analyses, with the reason each was archived. Their "production ready" assessments are superseded: protoCore is not production ready. |
| [docs/archive/design-specs/README.md](docs/archive/design-specs/README.md) | Historical design specifications (string redesign, GC survivor re-chain, heap allocation limit). |
