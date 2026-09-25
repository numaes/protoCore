#ifndef PROTO_CORE_CONFORMANCE_CATCH2_H
#define PROTO_CORE_CONFORMANCE_CATCH2_H

#include "protoCoreConformance.h"

#include <catch2/catch_all.hpp>

#include <iostream>
#include <string>

/**
 * The Catch2 counterpart of PROTOCORE_CONFORMANCE_GTEST, for the runtimes that
 * use Catch2 rather than GoogleTest.  protoCore's conformance library is
 * framework-free precisely so that this choice stays the embedder's.
 *
 * Usage, in the embedder's own test target:
 *
 *     #include <protoCoreConformanceCatch2.h>
 *     PROTOCORE_CONFORMANCE_CATCH2(MyHost)
 *
 * Catch2 has no parameterised-test instantiation that produces one ctest entry
 * per parameter the way gtest_discover_tests does, so the expansion uses
 * GENERATE(from_range(...)): catch_discover_tests registers ONE test, and Catch2
 * re-runs its body once per generated value.  Each conformance case therefore
 * appears as a separate SECTION-like run in the output with its own numbers, and
 * a failure names the case and the rule.
 *
 * As in the GoogleTest adapter: a fresh Host per case, NotApplicable becomes a
 * skip that names the missing capability and never a pass, and `detail` is
 * printed on every outcome including a pass.
 */
#define PROTOCORE_CONFORMANCE_CATCH2(HostType)                                 \
    TEST_CASE("protoCore embedder conformance", "[conformance]")               \
    {                                                                          \
        const char* id = GENERATE(                                            \
            from_range(::proto::conformance::caseIds()));                      \
        CAPTURE(id);                                                           \
        if (::proto::conformance::needsOwnProcess(id)) {                       \
            SKIP("must run in its own process under an external timeout "        \
                 "(abort or space-wide deadlock on failure): --case=" << id);    \
        }                                                                      \
        HostType host;                                                         \
        const ::proto::conformance::CaseResult r =                             \
            ::proto::conformance::runOne(host, id);                            \
        std::cout << ::proto::conformance::statusName(r.status)                \
                  << "  rule " << r.rule << "  " << r.id << "  "               \
                  << r.detail << std::endl;                                    \
        CHECK_FALSE(r.detail.empty());                                        \
        if (r.status == ::proto::conformance::Status::NotApplicable) {         \
            SKIP("NOT VERIFIED for this runtime: " << r.detail);                \
        }                                                                      \
        if (r.status == ::proto::conformance::Status::NeedsReview) {           \
            SKIP("NEEDS REVIEW: " << r.detail);                                \
        }                                                                      \
        CHECKED_ELSE(r.status != ::proto::conformance::Status::Fail) {         \
            FAIL_CHECK(r.detail);                                             \
        }                                                                      \
    }

#endif  // PROTO_CORE_CONFORMANCE_CATCH2_H
