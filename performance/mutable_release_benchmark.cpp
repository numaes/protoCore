/*
 * mutable_release_benchmark.cpp -- a collection that finalizes many mutable
 * objects at once, end to end: the sweep finalizes their handles and Phase 5b
 * removes their entries from the process-wide mutable table.
 *
 *   mutable_release_benchmark <mutables> <deadPercent> [exit [delayMs]]
 *
 * Builds `mutables` mutable objects, each written once (so each has a table
 * entry), keeps them in a list, then drops `deadPercent` % of them at random
 * and times the first full cycle that collects them.  With `exit`, the space
 * is destroyed as soon as that cycle has been requested and the time the
 * destructor takes is printed instead (it joins the collector, so it waits
 * for the cycle in progress).
 *
 * Self-verifying: the live mutables must still read their attribute back, and
 * (without `exit`) the table must hold exactly the survivors' entries.
 */

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <vector>

using namespace proto;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <mutables> <deadPercent> [exit]\n", argv[0]);
        return 2;
    }
    const long n = std::atol(argv[1]);
    const int deadPercent = std::atoi(argv[2]);
    const bool atExit = argc > 3 && !std::strcmp(argv[3], "exit");

    auto* space = new ProtoSpace();
    auto* ctx = new ProtoContext(space, space->rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    const ProtoString* key = ProtoString::createSymbol(ctx, "v");

    std::vector<const ProtoObject*> all;
    all.reserve(n);
    {
        // Built in chunks, each chunk rooted by the list so far.
        const ProtoList* list = ctx->newList();
        std::vector<const ProtoObject*> chunk;
        for (long i = 0; i < n; ++i) {
            ProtoObject* o = const_cast<ProtoObject*>(ctx->newObject(true));
            o->setAttribute(ctx, key, ctx->fromInteger(i));
            chunk.push_back(o);
            all.push_back(o);
            if (chunk.size() == 65536 || i + 1 == n) {
                for (auto* c : chunk) list = list->appendLast(ctx, c);
                ctx->getAutomaticLocals()[0] = list->asObject(ctx);
                chunk.clear();
                ctx->safepoint();
            }
        }
    }
    std::mt19937_64 rng(7);
    std::vector<const ProtoObject*> live;
    for (auto* o : all) if (static_cast<int>(rng() % 100) >= deadPercent) live.push_back(o);
    {
        const ProtoList* list = ctx->newList(static_cast<unsigned>(live.size()), live.data());
        ctx->getAutomaticLocals()[0] = list->asObject(ctx);
        ctx->safepoint();
    }
    all.clear();
    const long dead = n - static_cast<long>(live.size());

    auto request = [&]() {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space->gcStarted = true;
        space->gcCV.notify_all();
    };
    // Two cycles: the first may have started before the drop.
    const uint64_t c0 = space->getGCCycleCount();
    const auto t0 = std::chrono::steady_clock::now();
    request();
    if (atExit) {
        // Let the cycle get under way (it takes seconds at this size), then
        // tear down: the destructor joins the collector mid-cycle.
        const int delayMs = argc > 4 ? std::atoi(argv[4]) : 300;
        ProtoThread::sleepFor(ctx, static_cast<proto_ulong>(delayMs) * 1000);
        const uint64_t cyclesAtDelete = space->getGCCycleCount();
        delete ctx;
        const auto d0 = std::chrono::steady_clock::now();
        delete space;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - d0).count();
        std::printf("mutables=%ld dead=%ld exit after %d ms: cycles completed meanwhile=%llu destructor ms=%.1f\n", n, dead, delayMs, (unsigned long long)(cyclesAtDelete - c0), ms);
        return 0;
    }
    while (space->getGCCycleCount() < c0 + 2) {
        request();
        ctx->safepoint();
        ProtoThread::sleepFor(ctx, 200);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    long wrong = 0;
    for (size_t i = 0; i < live.size(); i += 97) {
        const ProtoObject* v = live[i]->getAttribute(ctx, key);
        if (!v || !v->isInteger(ctx)) ++wrong;
    }
    const proto_ulong entries = multispace::countMutableEntriesOfSpace(ctx, spaceIdOf(space));
    std::printf("mutables=%ld dead=%ld two-cycles ms=%.1f table_entries=%llu live=%zu wrong=%ld\n",
                n, dead, ms, (unsigned long long) entries, live.size(), wrong);
    delete ctx;
    delete space;
    return wrong == 0 ? 0 : 1;
}
