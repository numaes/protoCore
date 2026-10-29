/*
 * walk_pin_benchmark.cpp -- the cost of the callback walks over attribute
 * trees and sparse lists, single-threaded and under N concurrent ProtoThreads.
 *
 *   walk_pin_benchmark single <mode> <entries> <walks>
 *   walk_pin_benchmark threads <mode> <entries> <seconds> <n1,n2,...> [shared]
 *
 * Modes:
 *   mut  ProtoObject::processOwnAttributes on a MUTABLE object: pins the
 *        resolved state in the space's root set (one mutex pair per walk).
 *   imm  processOwnAttributes on an immutable object: no pin (control).
 *   sl   ProtoSparseList::processElements on a sparse list of `entries`
 *        entries (pins its receiver since the critical-section removal;
 *        allocated an iterator chain inside a critical section before).
 *
 * `threads` runs, for every N in the list, N ProtoThreads walking for
 * `seconds`; each walks its own receiver unless `shared` is given (then all
 * walk one).  Reports walks/s in total and per thread, and ns of wall time
 * per walk per thread.  Cycles per walk come from `perf stat -e cycles`
 * around a run of a single N, divided by the walks the run reports.
 *
 * Self-verifying: every walk must deliver exactly `entries` callbacks; a
 * short count exits non-zero.
 */

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace proto;

namespace {

enum class Mode { Mut, Imm, Sl };

Mode gMode = Mode::Mut;
int gEntries = 16;
std::atomic<bool> gGo{false};
std::atomic<bool> gStop{false};
std::atomic<int> gReady{0};
std::atomic<unsigned long long> gWalks{0};
std::atomic<unsigned long long> gCalls{0};
const ProtoObject* gShared = nullptr;

struct Counter { unsigned long long calls = 0; };

void onAttr(ProtoContext*, void* self, const ProtoString*, const ProtoObject*) {
    ++static_cast<Counter*>(self)->calls;
}
void onEntry(ProtoContext*, void* self, proto_ulong, const ProtoObject*) {
    ++static_cast<Counter*>(self)->calls;
}

// Builds the receiver for the mode in `ctx`.
const ProtoObject* buildReceiver(ProtoContext* ctx) {
    if (gMode == Mode::Sl) {
        const ProtoSparseList* list = ctx->newSparseList();
        for (int i = 0; i < gEntries; ++i)
            list = list->setAt(ctx, static_cast<proto_ulong>(i + 1), ctx->fromInteger(i));
        return list->asObject(ctx);
    }
    const ProtoObject* obj = ctx->newObject(gMode == Mode::Mut);
    for (int i = 0; i < gEntries; ++i) {
        const ProtoString* key = ProtoString::createSymbol(ctx, ("walk_attr_" + std::to_string(i)).c_str());
        obj = const_cast<ProtoObject*>(obj)->setAttribute(ctx, key, ctx->fromInteger(i));
    }
    return obj;
}

inline void walkOnce(ProtoContext* ctx, const ProtoObject* recv, Counter* c) {
    if (gMode == Mode::Sl)
        reinterpret_cast<const ProtoSparseList*>(recv)->processElements(ctx, c, onEntry);
    else
        recv->processOwnAttributes(ctx, c, onAttr);
}

const ProtoObject* walkerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoArguments*, const ProtoKeywordArguments*) {
    ProtoContext frame(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr, 1);
    const ProtoObject* recv = gShared ? gShared : buildReceiver(&frame);
    frame.getAutomaticLocals()[0] = recv;
    frame.safepoint();
    gReady.fetch_add(1);
    while (!gGo.load(std::memory_order_acquire)) ProtoThread::sleepFor(&frame, 100);
    Counter c;
    unsigned long long walks = 0;
    while (!gStop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 256; ++i) walkOnce(&frame, recv, &c);
        walks += 256;
        frame.safepoint();
    }
    gWalks.fetch_add(walks);
    gCalls.fetch_add(c.calls);
    return PROTO_NONE;
}

bool parseMode(const char* s) {
    if (!std::strcmp(s, "mut")) gMode = Mode::Mut;
    else if (!std::strcmp(s, "imm")) gMode = Mode::Imm;
    else if (!std::strcmp(s, "sl")) gMode = Mode::Sl;
    else return false;
    return true;
}

int runSingle(long long walks) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
    const ProtoObject* recv = buildReceiver(&ctx);
    ctx.getAutomaticLocals()[0] = recv;
    Counter c;
    const auto t0 = std::chrono::steady_clock::now();
    for (long long i = 0; i < walks; ++i) {
        walkOnce(&ctx, recv, &c);
        if ((i & 4095) == 0) ctx.safepoint();
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const unsigned long long expected = static_cast<unsigned long long>(walks) * gEntries;
    std::printf("single walks=%lld calls=%llu expected=%llu ns/walk=%.2f\n",
                walks, c.calls, expected, s * 1e9 / static_cast<double>(walks));
    return c.calls == expected ? 0 : 1;
}

int runThreads(double seconds, const std::vector<int>& counts, bool shared) {
    int rc = 0;
    std::printf("mode=%s entries=%d seconds=%.1f receivers=%s\n",
                gMode == Mode::Mut ? "mut" : gMode == Mode::Imm ? "imm" : "sl", gEntries, seconds,
                shared ? "shared" : "per-thread");
    std::printf("%8s %16s %16s %14s %10s\n", "threads", "walks/s total", "walks/s/thread", "ns/walk/thread", "verified");
    for (int n : counts) {
        ProtoSpace space;
        ProtoContext main(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 1);
        gShared = nullptr;
        if (shared) {
            gShared = buildReceiver(&main);
            main.getAutomaticLocals()[0] = gShared;
        }
        gGo = false; gStop = false; gReady = 0; gWalks = 0; gCalls = 0;
        std::vector<const ProtoThread*> threads;
        for (int i = 0; i < n; ++i)
            threads.push_back(space.newThread(&main, ProtoString::createSymbol(&main, ("walker" + std::to_string(i)).c_str()),
                                              walkerMain, nullptr, nullptr));
        while (gReady.load() < n) ProtoThread::sleepFor(&main, 1000);
        const auto t0 = std::chrono::steady_clock::now();
        gGo.store(true, std::memory_order_release);
        ProtoThread::sleepFor(&main, static_cast<proto_ulong>(seconds * 1e6));
        gStop.store(true);
        for (auto* t : threads) const_cast<ProtoThread*>(t)->join(&main);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const unsigned long long walks = gWalks.load();
        const bool ok = gCalls.load() == walks * static_cast<unsigned long long>(gEntries);
        if (!ok) rc = 1;
        const double perThread = static_cast<double>(walks) / s / n;
        std::printf("%8d %16.0f %16.0f %14.2f %10s  (walks=%llu)\n", n, walks / s, perThread,
                    1e9 / perThread, ok ? "yes" : "NO", walks);
        std::fflush(stdout);
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || !parseMode(argv[2])) {
        std::fprintf(stderr, "usage: %s single <mut|imm|sl> <entries> <walks>\n"
                             "       %s threads <mut|imm|sl> <entries> <seconds> <n1,n2,...> [shared]\n",
                     argv[0], argv[0]);
        return 2;
    }
    gEntries = std::atoi(argv[3]);
    if (!std::strcmp(argv[1], "single")) return runSingle(std::atoll(argv[4]));
    if (!std::strcmp(argv[1], "threads") && argc >= 6) {
        std::vector<int> counts;
        for (char* p = std::strtok(argv[5], ","); p; p = std::strtok(nullptr, ",")) counts.push_back(std::atoi(p));
        return runThreads(std::atof(argv[4]), counts, argc >= 7 && !std::strcmp(argv[6], "shared"));
    }
    return 2;
}
