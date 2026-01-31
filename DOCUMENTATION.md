# protoCore Documentation Index

**Last updated:** January 2026  
**Purpose:** Unified index of all protoCore documentation with references. Use this document to find the right doc for your need.

---

## 1. Entry points

| Document | Description | When to use |
|----------|-------------|-------------|
| [README.md](README.md) | Project overview, quick start, build, status, protoJS | First contact; build and run. |
| [DESIGN.md](DESIGN.md) | Architectural design, public API vs internal, memory model, object model | Understand architecture and contribution rules. |
| [COMPREHENSIVE_TECHNICAL_AUDIT_2026.md](COMPREHENSIVE_TECHNICAL_AUDIT_2026.md) | Full technical audit (architecture, implementation, tests, module system) | Current quality, metrics, and production readiness. |
| [docs/USER_GUIDE_UMD_MODULES.md](docs/USER_GUIDE_UMD_MODULES.md) | **User guide: generating a module for Unified Module Discovery** (steps, links to full guide and spec) | Create and register a module for UMD. |

---

## 2. Analysis and planning

| Document | Description | When to use |
|----------|-------------|-------------|
| [TECHNICAL_ANALYSIS.md](TECHNICAL_ANALYSIS.md) | Technical analysis: architecture, memory model, object model, codebase layout, build | High-level technical overview and recommendations. |
| [IMPROVEMENT_PLAN_2026.md](IMPROVEMENT_PLAN_2026.md) | Improvement plan: stability, developer experience, optional enhancements | Roadmap and maintenance. |
| [next_steps.md](next_steps.md) | Vision: ecosystem (protoPython, protoJS, browser), contribution | Strategic vision and ecosystem. |

---

## 3. Module system

| Document | Description | When to use |
|----------|-------------|-------------|
| [docs/USER_GUIDE_UMD_MODULES.md](docs/USER_GUIDE_UMD_MODULES.md) | **User guide: generating a module for Unified Module Discovery** — steps, quick reference, links to full guide and spec | How to generate/create a module for UMD. |
| [docs/MODULE_DISCOVERY.md](docs/MODULE_DISCOVERY.md) | Unified Module Discovery: resolution chain, ProviderRegistry, ProtoSpace::getImportModule, SharedModuleCache, FileSystemProvider, platform defaults | Specification and usage of the module system. |
| [docs/Structural description/guides/05_creating_modules.md](docs/Structural%20description/guides/05_creating_modules.md) | Full user guide: creating modules (ModuleProvider, registration, resolution chain, Greeter example) | Implement and register custom modules (detailed). |

---

## 4. Structural description (guides, architecture, tutorials)

| Document | Description | When to use |
|----------|-------------|-------------|
| [docs/Structural description/README.md](docs/Structural%20description/README.md) | Welcome and index for Structural description | Navigate guides, architecture, tutorials. |
| **Guides** | | |
| [01_quick_start.md](docs/Structural%20description/guides/01_quick_start.md) | Build and run; first program | Get started quickly. |
| [02_building_on_proto.md](docs/Structural%20description/guides/02_building_on_proto.md) | Building on protoCore (transpiler vs direct C++, integration) | Integrate protoCore into your app. |
| [03_contributing.md](docs/Structural%20description/guides/03_contributing.md) | Contributing | How to contribute. |
| [04_testing_user_guide.md](docs/Structural%20description/guides/04_testing_user_guide.md) | Running tests and coverage | Run tests and generate coverage. |
| [05_creating_modules.md](docs/Structural%20description/guides/05_creating_modules.md) | Creating modules (see §3) | Create custom ModuleProviders. |
| **Architecture** | | |
| [01_garbage_collector.md](docs/Structural%20description/architecture/01_garbage_collector.md) | GC design and behavior | Understand GC. |
| [02_mutability_model.md](docs/Structural%20description/architecture/02_mutability_model.md) | Mutability model | Understand mutability. |
| [03_object_model.md](docs/Structural%20description/architecture/03_object_model.md) | Object and type system | Understand object model. |
| [04_ffi_and_integration.md](docs/Structural%20description/architecture/04_ffi_and_integration.md) | FFI and C++ integration | FFI and embedding. |
| **Tutorials** | | |
| [01_building_a_repl.md](docs/Structural%20description/tutorials/01_building_a_repl.md) | Building a REPL | Tutorial: REPL. |
| [02_transpiling_python.md](docs/Structural%20description/tutorials/02_transpiling_python.md) | Transpiling Python | Tutorial: Python transpiler. |

---

## 5. Testing and build docs

| Document | Description | When to use |
|----------|-------------|-------------|
| [docs/TESTING.md](docs/TESTING.md) | Testing: CTest, parallel runs, caching, coverage, CI | Full testing documentation. |
| [docs/README.md](docs/README.md) | Building Sphinx/Doxygen docs | Build the doc site. |

---

## 6. Technical deep-dives

| Document | Description | When to use |
|----------|-------------|-------------|
| [docs/GarbageCollector.md](docs/GarbageCollector.md) | GC implementation: ProtoSpace, ProtoContext, DirtySegment, GC cycle | GC internals. |

---

## 7. Historical / reference (one-off audits and resolutions)

These documents describe completed one-off work. The canonical current state is in [COMPREHENSIVE_TECHNICAL_AUDIT_2026.md](COMPREHENSIVE_TECHNICAL_AUDIT_2026.md) and [README.md](README.md).

| Document | Description |
|----------|-------------|
| [API_COMPLETENESS_AUDIT_2026.md](API_COMPLETENESS_AUDIT_2026.md) | API completeness audit (36 methods implemented); status now in COMPREHENSIVE_TECHNICAL_AUDIT_2026. |
| [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) | Summary of GCBridge-related methods (ProtoString::asObject, etc.). |
| [PROTOCORE_BUFFER_API_RESOLUTION.md](PROTOCORE_BUFFER_API_RESOLUTION.md) | Buffer API implementation resolution for protoJS. |
| [GC_STRESS_TEST_FIX_ANALYSIS.md](GC_STRESS_TEST_FIX_ANALYSIS.md) | GC stress test fix (LargeAllocationReclamation). |

---

## 8. Superseded documents (removed or archived)

- **AUDIT_EXECUTIVE_SUMMARY.md** — Content merged into COMPREHENSIVE_TECHNICAL_AUDIT_2026.md; use that document for the executive summary.
- **docs/TechnicalAudit.md** — Superseded by COMPREHENSIVE_TECHNICAL_AUDIT_2026.md for architecture and audit findings.

---

## Quick links by role

- **New contributor:** README → docs/Structural description/README.md → 01_quick_start → 03_contributing.
- **Embedding protoCore:** README → DESIGN → docs/Structural description/guides/02_building_on_proto.
- **Module system / generating a UMD module:** docs/USER_GUIDE_UMD_MODULES.md → docs/MODULE_DISCOVERY.md → docs/Structural description/guides/05_creating_modules.md.
- **Quality and audit:** COMPREHENSIVE_TECHNICAL_AUDIT_2026.md.
- **Testing:** docs/TESTING.md, docs/Structural description/guides/04_testing_user_guide.md.
