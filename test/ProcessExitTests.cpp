// ProcessExitTests.cpp -- a process that exits while a space's collector runs.
//
// A runtime may leave a ProtoSpace alive when the process exits (a worker's
// space pinned by a thread that is never joined, a space owned by a static
// object, an embedder that calls exit() from a callback).  Its collector
// thread, and the sweep helpers, keep running while exit() runs the static
// destructors, and the OS kills them only afterwards.  So every process-wide
// structure the collector reaches must outlive static destruction, as the
// sweep helper pool always did ("never destroyed by a static destructor",
// collector-throughput spec 6.10).
//
// Until 2.14.3 three did not: the adaptive heap controller's registry of
// space states (adaptive::cellsPublished, reached from every chunk the sweep
// publishes, iterated it after its destruction: protoJS saw the crash), and
// the multi-space registry and destroyed-space list.  The condition variable
// that serialises the cycles of several spaces was destroyed under a waiting
// collector as well.
//
// The cases:
//   * deterministic: an atexit handler registered BEFORE the structures are
//     first used runs AFTER their static destructors (the C++ rule: reverse
//     order of registration), and calls into them the way a collector would;
//   * a stress run: the process exits while a collector sweeps garbage of
//     several allocating threads in several spaces.  Probabilistic by nature
//     (it needs the collector inside one of those structures at the moment
//     of destruction); the deterministic case is the gate.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../core/AdaptiveHeap.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

using namespace proto;

namespace {

ProtoSpace* gExitSpace = nullptr;

// Runs after the static destructors of everything first used after it was
// registered.
void callIntoTheRegistriesAtExit() {
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        // What the sweep calls for every chunk it publishes.
        adaptive::cellsPublished(gExitSpace);
        (void) adaptive::isEnabled(gExitSpace);
    }
    // What a cycle's Phase 5b calls.
    std::vector<proto_ulong> refs;
    multispace::appendRefsOfDestroyedSpaces(refs);
    std::fprintf(stderr, "AT_EXIT_HANDLER_DONE\n");
    std::fflush(stderr);
}

}  // namespace

TEST(ProcessExitDeathTest, RegistriesTheCollectorReachesOutliveStaticDestruction) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT({
        // Registered first, so it runs last.
        std::atexit(callIntoTheRegistriesAtExit);
        // First use of the adaptive registry: a space registers its state.
        gExitSpace = new ProtoSpace();
        AdaptiveHeapConfig c;
        c.hardCells = 4000000;
        gExitSpace->enableAdaptiveHeap(c);
        // First use of the destroyed-space list: a space that dies.  Its id
        // stays recorded until a later cycle takes it.
        { ProtoSpace gone; }
        std::exit(0);
    }, ::testing::ExitedWithCode(0), "AT_EXIT_HANDLER_DONE");
}

namespace {

std::atomic<bool> gChurnStarted{false};

const ProtoObject* churnForever(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                const ProtoArguments*, const ProtoKeywordArguments*) {
    ProtoThread* self = const_cast<ProtoThread*>(ctx->thread);
    for (;;) {
        {
            ProtoContext garbage(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
            for (int i = 0; i < 100; ++i) (void) garbage.newObject(false);
        }
        gChurnStarted = true;
        self->synchToGC();
    }
}

}  // namespace

// Two spaces under the controller with small budgets, so their collectors
// run back to back, each fed by a thread that allocates garbage forever; the
// main thread exits the process while they run.  Repeated a few times per
// child to widen the window.
TEST(ProcessExitDeathTest, ExitWhileCollectorsRun) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    for (int run = 0; run < 5; ++run) {
        EXPECT_EXIT({
            gChurnStarted = false;
            ProtoContext* firstRoot = nullptr;
            for (int s = 0; s < 2; ++s) {
                ProtoSpace* space = new ProtoSpace();   // never destroyed
                AdaptiveHeapConfig c;
                c.hardCells = 600000;
                space->enableAdaptiveHeap(c);
                ProtoContext* root = space->rootContext;
                if (!firstRoot) firstRoot = root;
                (void) space->newThread(root, ProtoString::createSymbol(root, "exit-churn"),
                                        churnForever, nullptr, nullptr);
            }
            {
                // The main thread leaves the quorum of both spaces while it
                // waits, so their collections can stop the world.
                ProtoContext::UnmanagedScope parked(firstRoot);
                while (!gChurnStarted.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            std::exit(0);
        }, ::testing::ExitedWithCode(0), "") << "run " << run;
    }
}
