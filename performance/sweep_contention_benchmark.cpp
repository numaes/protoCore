/*
 * sweep_contention_benchmark.cpp -- the sweep's cost per cell under the
 * conditions that separate the hypotheses of
 * docs/specs/2026-10-03-collector-throughput-design.md, section 5.2.
 *
 * One space.  Each scenario builds a fixed garbage set of G objects (about
 * 2G cells) in segments of about 6 cells (short-lived contexts of 6 objects, as a runtime's
 * calls submit them), runs one collection cycle, and prints the sweep's
 * nanoseconds per swept cell from the collector's always-on cycle measures
 * (adaptive::lastCycleMeasures).
 *
 *   S0  no other thread during the sweep                     baseline
 *   S1  N threads streaming private memory (no protoCore)    H2: loaded DRAM
 *   S2  N protoCore threads submitting and destroying small
 *       contexts in a loop                                   H3: shared lines
 *   S3  N protoCore threads reading the survivors            H1/H5: live lines
 *       (S3b: the same survivors, no readers)
 *   S4  garbage built by N protoCore threads, then idle       H1: dead lines
 *   S5  garbage in fresh memory (the space's first cells)    H2: contiguity
 *       against S0's recycled memory
 *
 *   sweep_contention_benchmark [garbageCells] [threads] [reps]
 *
 * Defaults: 3,000,000 objects, 6 threads, 3 repetitions (median reported).
 * Self-verifying: every measured cycle must have swept and freed at least the
 * garbage it built; a short count exits non-zero.
 */

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include "../core/AdaptiveHeap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace proto;

namespace {

// Three objects per short-lived context: about 6 cells per submitted
// segment, the mean measured on runtime workloads (an object is 2 cells).
constexpr int kObjectsPerSegment = 3;

std::atomic<bool> gStop{false};
std::atomic<int> gStarted{0};
ProtoRootSet* gRoots = nullptr;
ProtoRootSet::Handle gLive = ProtoRootSet::kNullHandle;
int gPerThreadGarbage = 0;
bool gFailed = false;

// Garbage of `cells` objects in contexts of kObjectsPerSegment objects.
void buildGarbage(ProtoSpace& space, ProtoContext* parent, int cells) {
    for (int done = 0; done < cells; done += kObjectsPerSegment) {
        {
            ProtoContext sub(&space, parent, nullptr, nullptr, nullptr, nullptr);
            for (int j = 0; j < kObjectsPerSegment; ++j) (void) sub.newObject(false);
        }
        if ((done & 4095) == 0) parent->safepoint();
    }
}

std::uint64_t completedCycles(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    return adaptive::cyclesCompleted(&space);
}

// Request one cycle and wait, at safepoints, until it has completed.
adaptive::CycleMeasures runCycle(ProtoSpace& space) {
    const std::uint64_t before = completedCycles(space);
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        if (!space.gcStarted) {
            adaptive::noteCycleRequested(&space);
            space.gcStarted = true;
        }
        space.gcCV.notify_all();
    }
    while (completedCycles(space) == before) {
        space.rootContext->safepoint();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    return adaptive::lastCycleMeasures(&space);
}

// --- Background loads -----------------------------------------------------

void streamPrivateMemory() {
    // 64 MiB per thread: far beyond the caches, so every pass loads DRAM.
    std::vector<std::uint64_t> buf(8u << 20, 1);
    std::uint64_t sum = 0;
    gStarted.fetch_add(1);
    while (!gStop.load(std::memory_order_relaxed)) {
        for (std::size_t i = 0; i < buf.size(); i += 8) {
            sum += buf[i];
            buf[i] = sum;
        }
    }
    if (sum == 42) std::puts("");
}

const ProtoObject* submitLoop(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoList*, const ProtoSparseList*) {
    gStarted.fetch_add(1);
    while (!gStop.load(std::memory_order_relaxed)) {
        for (int k = 0; k < 64; ++k) {
            ProtoContext sub(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
            (void) sub.newObject(false);
        }
        ctx->safepoint();
    }
    return PROTO_NONE;
}

const ProtoObject* readSurvivors(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                 const ProtoList*, const ProtoSparseList*) {
    gStarted.fetch_add(1);
    std::uint64_t seen = 0;
    while (!gStop.load(std::memory_order_relaxed)) {
        const ProtoList* list = gRoots->resolve(gLive)->asList(ctx);
        const unsigned long n = list->getSize(ctx);
        for (unsigned long i = 0; i < n; ++i) {
            if (list->getAt(ctx, static_cast<int>(i))) ++seen;
            if ((i & 1023) == 0) {
                if (gStop.load(std::memory_order_relaxed)) break;
                ctx->safepoint();
            }
        }
        ctx->safepoint();
    }
    return seen == 0xFFFFFFFFFFFFFFFFull ? nullptr : PROTO_NONE;
}

const ProtoObject* buildGarbageWorker(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                      const ProtoList*, const ProtoSparseList*) {
    buildGarbage(*ctx->space, ctx, gPerThreadGarbage);
    return PROTO_NONE;
}

std::vector<const ProtoThread*> startProtoThreads(ProtoSpace& space, int n, ProtoMethod fn) {
    std::vector<const ProtoThread*> threads;
    ProtoContext* root = space.rootContext;
    for (int i = 0; i < n; ++i)
        threads.push_back(space.newThread(root, ProtoString::createSymbol(root, "bench-worker"),
                                          fn, nullptr, nullptr));
    return threads;
}

void joinProtoThreads(ProtoSpace& space, std::vector<const ProtoThread*>& threads) {
    for (const ProtoThread* t : threads) const_cast<ProtoThread*>(t)->join(space.rootContext);
    threads.clear();
}

void waitStarted(ProtoSpace& space, int n) {
    while (gStarted.load() < n) {
        space.rootContext->safepoint();
        std::this_thread::yield();
    }
}

// A live list of `elements` objects in the root set, built in chunks so its
// cells are in submitted segments (candidates that survive).
void buildLive(ProtoSpace& space, int elements) {
    ProtoContext* root = space.rootContext;
    if (!gRoots) gRoots = space.createRootSet("sweep-contention");
    if (gLive != ProtoRootSet::kNullHandle) gRoots->remove(gLive);
    gLive = gRoots->add(root->newList()->asObject(root));
    for (int done = 0; done < elements;) {
        ProtoContext sub(&space, root, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* list = gRoots->resolve(gLive)->asList(&sub);
        const int chunk = std::min(5000, elements - done);
        for (int i = 0; i < chunk; ++i) list = list->appendLast(&sub, sub.newObject(false));
        gRoots->remove(gLive);
        gLive = gRoots->add(list->asObject(&sub));
        done += chunk;
    }
}

void dropLive() {
    if (gRoots && gLive != ProtoRootSet::kNullHandle) {
        gRoots->remove(gLive);
        gLive = ProtoRootSet::kNullHandle;
    }
}

struct Result {
    double nsPerCell;
    proto_ulong swept;
    proto_ulong freed;
};

Result measure(const char* name, const adaptive::CycleMeasures& m, proto_ulong garbage) {
    Result r{m.sweptCells ? static_cast<double>(m.sweepNanos) / static_cast<double>(m.sweptCells) : 0.0,
             m.sweptCells, m.freedCells};
    const bool ok = m.sweptCells >= garbage && m.freedCells >= garbage;
    if (!ok) gFailed = true;
    std::printf("  %-4s sweep=%8.2f ms swept=%9lu freed=%9lu segs=%8lu ns/cell=%6.1f mark=%7.2f ms%s\n",
                name, m.sweepNanos / 1e6, (unsigned long) m.sweptCells, (unsigned long) m.freedCells,
                (unsigned long) m.sweptSegments, r.nsPerCell, m.markNanos / 1e6, ok ? "" : "  ** SHORT **");
    std::fflush(stdout);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const int garbage = argc > 1 ? std::atoi(argv[1]) : 3000000;
    const int threads = argc > 2 ? std::atoi(argv[2]) : 6;
    const int reps = argc > 3 ? std::atoi(argv[3]) : 3;
    std::printf("sweep_contention_benchmark garbage=%d threads=%d reps=%d\n", garbage, threads, reps);
    std::map<std::string, std::vector<double>> ns;

    for (int rep = 0; rep < reps; ++rep) {
        ProtoSpace space;
        ProtoContext* root = space.rootContext;
        std::printf("rep %d\n", rep);

        // S5: the space's first garbage, in memory fresh from the OS.
        buildGarbage(space, root, garbage);
        ns["S5"].push_back(measure("S5", runCycle(space), garbage).nsPerCell);
        (void) runCycle(space);   // drain: the pen holds only true survivors

        // S0: recycled memory, nothing else running.
        buildGarbage(space, root, garbage);
        ns["S0"].push_back(measure("S0", runCycle(space), garbage).nsPerCell);

        // S1: N threads streaming private memory during the sweep.
        buildGarbage(space, root, garbage);
        {
            gStop = false; gStarted = 0;
            std::vector<std::thread> ts;
            for (int i = 0; i < threads; ++i) ts.emplace_back(streamPrivateMemory);
            waitStarted(space, threads);
            ns["S1"].push_back(measure("S1", runCycle(space), garbage).nsPerCell);
            gStop = true;
            for (auto& t : ts) t.join();
        }

        // S2: N protoCore threads submitting and destroying contexts.
        buildGarbage(space, root, garbage);
        {
            gStop = false; gStarted = 0;
            auto ts = startProtoThreads(space, threads, submitLoop);
            waitStarted(space, threads);
            ns["S2"].push_back(measure("S2", runCycle(space), garbage).nsPerCell);
            gStop = true;
            joinProtoThreads(space, ts);
        }
        (void) runCycle(space);   // collect S2's own garbage

        // S3b / S3: survivors in the candidate set, without and with readers.
        const int liveElements = garbage / 4;
        buildLive(space, liveElements);
        (void) runCycle(space);   // the live list's build garbage
        buildGarbage(space, root, garbage);
        ns["S3b"].push_back(measure("S3b", runCycle(space), garbage).nsPerCell);
        buildGarbage(space, root, garbage);
        {
            gStop = false; gStarted = 0;
            auto ts = startProtoThreads(space, threads, readSurvivors);
            waitStarted(space, threads);
            ns["S3"].push_back(measure("S3", runCycle(space), garbage).nsPerCell);
            gStop = true;
            joinProtoThreads(space, ts);
        }
        dropLive();
        (void) runCycle(space);   // the live list is garbage now
        (void) runCycle(space);

        // S4: the garbage built by N threads, which then exit.
        {
            gPerThreadGarbage = garbage / threads;
            auto ts = startProtoThreads(space, threads, buildGarbageWorker);
            joinProtoThreads(space, ts);
            ns["S4"].push_back(measure("S4", runCycle(space),
                                       static_cast<proto_ulong>(gPerThreadGarbage) * threads).nsPerCell);
        }
        gRoots = nullptr;
        gLive = ProtoRootSet::kNullHandle;
    }

    std::printf("median ns per swept cell (%d reps):", reps);
    for (const char* k : {"S0", "S1", "S2", "S3b", "S3", "S4", "S5"}) {
        std::vector<double> v = ns[k];
        std::sort(v.begin(), v.end());
        std::printf(" %s=%.1f", k, v.empty() ? 0.0 : v[v.size() / 2]);
    }
    std::printf("\n%s\n", gFailed ? "verified=no" : "verified=yes");
    return gFailed ? 1 : 0;
}
