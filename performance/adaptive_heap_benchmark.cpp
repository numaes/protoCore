/*
 * adaptive_heap_benchmark.cpp -- heap size, cycles and RSS of an
 * allocation-heavy workload, with the adaptive heap controller or with a
 * fixed limit.
 *
 * The workload is shaped like an interpreter: a live set held in a root set,
 * and a loop of "calls", each a short-lived context that allocates objects
 * and builds a small list (all garbage once the call returns), with a
 * safepoint between calls, as runtimes place one between bytecodes.
 *
 *   adaptive_heap_benchmark adaptive [live] [calls] [perCall]
 *   adaptive_heap_benchmark fixed <hardCells> [live] [calls] [perCall]
 *
 * Defaults: live 100000 elements (about 200,000 cells), 200,000 calls of 100
 * objects (about 40,000,000 cells of garbage).  `fixed 10485760` is the
 * 640 MB limit runtimes used to set.  PROTOCORE_HEAP_TRACE=1 prints the
 * controller's per-cycle line.
 *
 * Self-verifying: it prints the work done and exits non-zero when the live
 * set did not survive intact or the number of calls is short.
 */

#include "../headers/protoCore.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace proto;

static long maxRssKiB() {
#if defined(__linux__)
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;            // KiB on Linux
#elif defined(__APPLE__)
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss / 1024;     // bytes on macOS
#else
    return -1;
#endif
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s adaptive|fixed <hard> [live] [calls] [perCall]\n", argv[0]);
        return 2;
    }
    const bool adaptive = std::strcmp(argv[1], "adaptive") == 0;
    int arg = 2;
    int hard = 0;
    if (!adaptive) {
        if (argc < 3) return 2;
        hard = std::atoi(argv[arg++]);
    }
    const int live = argc > arg ? std::atoi(argv[arg++]) : 100000;
    const int calls = argc > arg ? std::atoi(argv[arg++]) : 200000;
    const int perCall = argc > arg ? std::atoi(argv[arg++]) : 100;

    ProtoSpace space;
    if (adaptive) space.enableAdaptiveHeap();
    else space.setHeapLimits(0, hard);
    ProtoContext* root = space.rootContext;

    const auto t0 = std::chrono::steady_clock::now();

    // The live set: a list in a root set, built in chunks.
    ProtoRootSet* rs = space.createRootSet("adaptive-heap-benchmark");
    ProtoRootSet::Handle h = rs->add(root->newList()->asObject(root));
    for (int done = 0; done < live;) {
        ProtoContext sub(&space, root, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* list = rs->resolve(h)->asList(&sub);
        const int chunk = live - done < 10000 ? live - done : 10000;
        for (int i = 0; i < chunk; ++i) list = list->appendLast(&sub, sub.fromInteger(done + i));
        rs->remove(h);
        h = rs->add(list->asObject(&sub));
        done += chunk;
        root->safepoint();
    }

    // The calls.
    long long completed = 0;
    for (int c = 0; c < calls; ++c) {
        {
            ProtoContext call(&space, root, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* tmp = call.newList();
            for (int j = 0; j < perCall; ++j) {
                const ProtoObject* o = call.newObject(false);
                if ((j & 7) == 0) tmp = tmp->appendLast(&call, o);
            }
            if (tmp->getSize(&call) != static_cast<proto_ulong>((perCall + 7) / 8)) {
                std::fprintf(stderr, "call %d built a wrong list\n", c);
                return 1;
            }
        }
        root->safepoint();
        ++completed;
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Verify the live set.
    const ProtoList* list = rs->resolve(h)->asList(root);
    const proto_ulong n = list->getSize(root);
    bool ok = n == static_cast<proto_ulong>(live) && completed == calls;
    if (ok && live > 0) {
        ok = list->getAt(root, 0)->asLong(root) == 0
          && list->getAt(root, live - 1)->asLong(root) == live - 1;
    }

    const AdaptiveHeapStats s = space.adaptiveHeapStats();
    std::printf("mode=%s live=%d calls=%lld perCall=%d seconds=%.3f cycles=%llu "
                "heapCells=%d heapMiB=%.1f softCells=%lu hardCells=%lu L=%lu p=%.4f "
                "maxRssMiB=%.1f verified=%s\n",
                adaptive ? "adaptive" : "fixed", live, completed, perCall, seconds,
                (unsigned long long) space.getGCCycleCount(), space.heapSize,
                space.heapSize * 64.0 / (1024 * 1024), (unsigned long) s.softCells,
                (unsigned long) s.hardCells, (unsigned long) s.liveCellsLastCycle,
                s.lastPressure, maxRssKiB() / 1024.0, ok ? "yes" : "NO");
    space.destroyRootSet(rs);
    return ok ? 0 : 1;
}
