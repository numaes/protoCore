/*
 * bulk_remove_benchmark.cpp -- the cost of the collector's Phase 5b on its
 * own: removing the entries of dead mutable objects from the mutable table's
 * shard trees with sparseListRemoveSorted, the way
 * releaseFinalizedMutableEntries does (one call per shard, keys sorted).
 *
 *   bulk_remove_benchmark <entries> <pattern> [reps]
 *
 * `entries` live entries are spread over the 256 shards as the table spreads
 * them (ref r goes to shard r % 256, refs are consecutive).  `pattern` picks
 * the dead ones:
 *   all     every entry (a whole program's mutables die, e.g. at exit)
 *   rand90  each ref with probability 9/10
 *   rand50  each ref with probability 1/2
 *   rand10  each ref with probability 1/10
 *   old50   the oldest half (a generational pattern)
 *   rand1   each ref with probability 1/100
 *
 * Prints the time per call batch and the cells allocated, and verifies every
 * result's size: a wrong size exits non-zero.
 */

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include "../core/SparseListAlgorithms.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace proto;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <entries> <all|rand90|rand50|rand10|old50|rand1> [reps]\n", argv[0]);
        return 2;
    }
    const proto_ulong entries = std::strtoull(argv[1], nullptr, 10);
    const char* pattern = argv[2];
    const int reps = argc > 3 ? std::atoi(argv[3]) : 3;
    constexpr proto_ulong kShards = ProtoSpace::MUTABLE_ROOT_SHARDS;

    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    const ProtoObject* value = ctx.fromInteger(1);

    // Shard trees, built balanced from sorted keys.
    std::vector<const ProtoSparseList*> roots(kShards, nullptr);
    std::vector<std::vector<proto_ulong>> dead(kShards);
    std::vector<proto_ulong> liveAfter(kShards, 0);
    std::mt19937_64 rng(12345);
    {
        std::vector<std::vector<proto_ulong>> keys(kShards);
        for (proto_ulong r = 1; r <= entries; ++r) {
            const proto_ulong s = r % kShards;
            keys[s].push_back(r);
            bool die = false;
            if (!std::strcmp(pattern, "all")) die = true;
            else if (!std::strcmp(pattern, "rand90")) die = rng() % 10 != 0;
            else if (!std::strcmp(pattern, "rand50")) die = (rng() & 1) == 0;
            else if (!std::strcmp(pattern, "rand10")) die = rng() % 10 == 0;
            else if (!std::strcmp(pattern, "rand1")) die = rng() % 100 == 0;
            else if (!std::strcmp(pattern, "old50")) die = r <= entries / 2;
            else { std::fprintf(stderr, "unknown pattern\n"); return 2; }
            if (die) dead[s].push_back(r); else ++liveAfter[s];
        }
        for (proto_ulong s = 0; s < kShards; ++s) {
            if (keys[s].empty()) { roots[s] = ctx.newSparseList(); continue; }
            std::vector<const ProtoObject*> values(keys[s].size(), value);
            const auto* t = sparse_avl::buildSorted<ProtoSparseListImplementation>(
                &ctx, keys[s].data(), values.data(), 0, keys[s].size());
            roots[s] = t->asSparseList(&ctx);
        }
    }
    proto_ulong deadCount = 0;
    for (auto& d : dead) deadCount += d.size();

    int rc = 0;
    for (int rep = 0; rep < reps; ++rep) {
        // A fresh context per repetition so its cell count is this run's.
        ProtoContext run(&space, &ctx, nullptr, nullptr, nullptr, nullptr);
        const auto t0 = std::chrono::steady_clock::now();
        proto_ulong cells = 0;
        for (proto_ulong s = 0; s < kShards; ++s) {
            const proto_ulong before = run.allocatedCellsCount;
            const ProtoSparseList* out =
                sparseListRemoveSorted(&run, roots[s], dead[s].data(), dead[s].size());
            cells += run.allocatedCellsCount - before;
            if (out->getSize(&run) != liveAfter[s]) {
                std::fprintf(stderr, "shard %llu: size %llu, expected %llu\n",
                             (unsigned long long) s, (unsigned long long) out->getSize(&run),
                             (unsigned long long) liveAfter[s]);
                rc = 1;
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("entries=%llu pattern=%s dead=%llu rep=%d ms=%.1f ns/dead=%.1f cells=%llu cells/dead=%.2f\n",
                    (unsigned long long) entries, pattern, (unsigned long long) deadCount, rep, ms,
                    deadCount ? ms * 1e6 / deadCount : 0.0, (unsigned long long) cells,
                    deadCount ? double(cells) / deadCount : 0.0);
        std::fflush(stdout);
    }
    return rc;
}
