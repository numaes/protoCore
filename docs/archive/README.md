# Archive

This directory holds dated documents kept for reference. Each one records the state of protoCore, or a plan for it, at the time it was written; they are not maintained and may not match the current code. Several of them describe protoCore as production ready: that assessment is superseded, and protoCore is not production ready. Current documentation is indexed in [DOCUMENTATION.md](../../DOCUMENTATION.md).

| File | Reason for archiving |
|------|----------------------|
| [COMPREHENSIVE_TECHNICAL_AUDIT_2026.md](COMPREHENSIVE_TECHNICAL_AUDIT_2026.md) | Technical audit dated April 2026; its metrics and production-readiness assessment are no longer current. |
| [IMPROVEMENT_PLAN_2026.md](IMPROVEMENT_PLAN_2026.md) | Improvement plan dated January 2026, built on the superseded production-readiness assessment. |
| [API_COMPLETENESS_AUDIT_2026.md](API_COMPLETENESS_AUDIT_2026.md) | One-off record (January 2026) of the 36 missing `protoCore.h` methods that were implemented. |
| [PROTOCORE_BUFFER_API_RESOLUTION.md](PROTOCORE_BUFFER_API_RESOLUTION.md) | One-off record (January 2026) of the `ProtoByteBuffer` and buffer factory methods added for protoJS's Buffer module. |
| [GC_STRESS_TEST_FIX_ANALYSIS.md](GC_STRESS_TEST_FIX_ANALYSIS.md) | One-off analysis (January 2026) of the `GCStressTest.LargeAllocationReclamation` fix; the survivor leak behind it was later addressed by the [GC survivor re-chain design](design-specs/2026-05-03-gc-survivor-rechain.md). |
| [design-specs/](design-specs/README.md) | Historical design specifications written during development. |
