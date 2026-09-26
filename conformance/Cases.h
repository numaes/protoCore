// Cases.h -- the internal case registry.
//
// Every case is a plain function of a Host.  The table lives in Runner.cpp so
// that the order in which cases run is stated in exactly one place: it matters,
// because gc.host_stress is only meaningful once gc.young_submitted has been
// observed (protoST's MailboxCursor::adopt bug was UNREACHABLE while the
// collector reclaimed nothing).
#ifndef PROTO_CORE_CONFORMANCE_CASES_H
#define PROTO_CORE_CONFORMANCE_CASES_H

#include "../headers/protoCoreConformance.h"

namespace proto { namespace conformance {

using CaseFn = CaseResult (*)(Host&);

struct CaseEntry
{
    const char* id;
    unsigned    rule;
    bool        ownProcess; ///< failure destroys the run (abort OR deadlock)
    bool        kernelOnly; ///< audits protoCore, not the embedder (see isKernelCase)
    CaseFn      fn;
};

/// CaseGC.cpp
CaseResult caseYoungSubmitted(Host& host);
CaseResult caseTransientReclaimed(Host& host);
CaseResult caseHostStress(Host& host);

/// CaseThreads.cpp
CaseResult caseThreadRegistered(Host& host);
CaseResult caseQuorumCompletes(Host& host);
CaseResult caseJoinParks(Host& host);

/// CaseHeap.cpp
CaseResult caseCeilingProgress(Host& host);

/// CaseExternal.cpp
CaseResult casePerennialNeverFinalized(Host& host);
CaseResult caseExternalBytesAccounted(Host& host);

/// CaseSymbols.cpp
CaseResult caseFastPathKeyHits(Host& host);

/// CaseModules.cpp
CaseResult caseModuleRootSurvivesCycle(Host& host);
CaseResult caseModuleAliasRejected(Host& host);

/// CaseMutables.cpp
CaseResult caseMutableGraphCycles(Host& host);

/// Shared helper: a NotApplicable result naming the capability that was missing.
CaseResult unavailable(const char* id, unsigned rule, const char* capability,
                       const char* why);

}}  // namespace proto::conformance

#endif
