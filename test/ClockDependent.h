// ClockDependent.h -- the cases whose verdict is partly the machine's.
//
// protoCore's CI rule (.github/workflows/ci.yml): nothing whose verdict is a
// clock may fail a build.  The cases listed in CLOCK_DEPENDENT_TESTS there run
// in an informational job of protoCore's own CI and never gate.
//
// A runtime that builds protoCore with add_subdirectory runs this suite in
// ITS ctest, where it gates, and cannot know which cases are informational:
// protoPython's CI failed first attempts on AdaptiveHeapSteady and
// AdaptiveHeapStorm cases in October 2026, with no code change.  So when
// protoCore is not the top-level project these cases skip themselves (and
// say so) unless PROTOCORE_CLOCK_DEPENDENT_TESTS=ON.  In protoCore's own
// tree they run as before.
//
// Every case that calls PROTOCORE_CLOCK_DEPENDENT() must be listed in
// CLOCK_DEPENDENT_TESTS of ci.yml and cross-platform.yml, and vice versa.

#ifndef PROTOCORE_TEST_CLOCK_DEPENDENT_H
#define PROTOCORE_TEST_CLOCK_DEPENDENT_H

#include <gtest/gtest.h>

#if defined(PROTOCORE_SKIP_CLOCK_DEPENDENT_TESTS)
#define PROTOCORE_CLOCK_DEPENDENT()                                                   \
    GTEST_SKIP() << "clock-dependent case: informational in protoCore's own CI, "     \
                    "not run when protoCore is a subproject "                          \
                    "(-DPROTOCORE_CLOCK_DEPENDENT_TESTS=ON runs it)"
#else
#define PROTOCORE_CLOCK_DEPENDENT() (void) 0
#endif

#endif
