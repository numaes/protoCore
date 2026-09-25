#ifndef PROTO_CORE_CONFORMANCE_GTEST_H
#define PROTO_CORE_CONFORMANCE_GTEST_H

#include "protoCoreConformance.h"

#include <gtest/gtest.h>

#include <cctype>
#include <string>

/**
 * Expands ONE GoogleTest case per conformance case, so a failure names the rule
 * it violated instead of hiding inside one aggregate test.  An aggregate would
 * also stop at the first failure, and the whole value of a first run is the
 * COMPLETE list.
 *
 * Usage, in the embedder's own test target:
 *
 *     #include <protoCoreConformanceGTest.h>
 *     PROTOCORE_CONFORMANCE_GTEST(MyHost)
 *
 * where MyHost is default-constructible and derives from
 * proto::conformance::Host.
 *
 * Three deliberate choices:
 *
 *  * A fresh Host per case.  A shared Host would let one case's heap state
 *    decide another's verdict, and the GC cases measure heap state.
 *  * NotApplicable becomes a SKIP with the missing capability named, never a
 *    pass.  A skipped case in the report is the suite telling the maintainer
 *    exactly which rule is unverified for this runtime.
 *  * `detail` is recorded on every outcome, pass included.  A run whose details
 *    are all empty is a broken harness that looks like a clean suite.
 */
#define PROTOCORE_CONFORMANCE_GTEST(HostType)                                  \
    class HostType##Conformance                                                \
        : public ::testing::TestWithParam<const char*> {};                     \
    TEST_P(HostType##Conformance, ObeysRule) {                                 \
        if (::proto::conformance::isAbortingCase(GetParam()))                  \
            GTEST_SKIP() << "failure mode is std::abort(); run through the "    \
                            "isolate binary: --case=" << GetParam();           \
        HostType host;                                                         \
        const ::proto::conformance::CaseResult r =                             \
            ::proto::conformance::runOne(host, GetParam());                    \
        RecordProperty("rule", static_cast<int>(r.rule));                      \
        RecordProperty("detail", r.detail);                                    \
        std::cout << ::proto::conformance::statusName(r.status)                \
                  << "  rule " << r.rule << "  " << r.id << "  "               \
                  << r.detail << std::endl;                                    \
        EXPECT_NE(r.status, ::proto::conformance::Status::Fail) << r.detail;    \
        EXPECT_FALSE(r.detail.empty())                                        \
            << "the case reported no numbers, which is a broken harness";      \
        if (r.status == ::proto::conformance::Status::NotApplicable)           \
            GTEST_SKIP() << "NOT VERIFIED for this runtime: " << r.detail;     \
        if (r.status == ::proto::conformance::Status::NeedsReview)             \
            GTEST_SKIP() << "NEEDS REVIEW: " << r.detail;                      \
    }                                                                          \
    INSTANTIATE_TEST_SUITE_P(                                                  \
        conformance, HostType##Conformance,                                    \
        ::testing::ValuesIn(::proto::conformance::caseIds()),                  \
        [](const ::testing::TestParamInfo<const char*>& i) {                   \
            std::string n(i.param);                                            \
            for (char& c : n)                                                  \
                if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';      \
            return n;                                                          \
        });

#endif  // PROTO_CORE_CONFORMANCE_GTEST_H
