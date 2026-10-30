/*
 * symbol_intern_benchmark.cpp -- the cost of interning and looking up symbols
 * as the process-wide symbol table grows, and lookups under concurrent
 * inserts.
 *
 *   symbol_intern_benchmark fill <total> <checkpoint> [<checkpoint> ...]
 *   symbol_intern_benchmark readers <tableSize> <seconds> <writers> <r1,r2,...>
 *
 * fill: interns `total` distinct spellings (14 bytes each, never the inline
 * form) through ProtoString::createSymbol.  At each checkpoint -- a table
 * holding that many of them -- it reports:
 *
 *   fill_us     mean cost of each intern since the previous checkpoint
 *   new_us      mean cost of interning 1,000 new spellings
 *   lookup_us   mean cost of createSymbol on 1,000 spellings already interned
 *               (spread over the whole table)
 *
 * and, where the table is the write-once trie, its index memory at the end.
 * Self-verifying, exit status 1 on any failure: createSymbol of an interned
 * spelling returns the pointer it returned the first time (all of them, at
 * the end); no two spellings share a pointer; the table grew by exactly the
 * number of spellings interned.  The fill is "linear" when the per-symbol cost
 * of the last interval is within 3x that of the second (the first is warm-up);
 * the verdict is printed, and fails the run only with `strict` appended.
 *
 * readers: fills the table to `tableSize`, then for each reader count runs, for
 * `seconds`, that many ProtoThreads calling createSymbol on random interned
 * spellings while `writers` ProtoThreads intern new ones without pause.  Every
 * lookup is checked against the pointer recorded at fill time.  Prints total
 * lookups per second and the writers' interns per second.
 */

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using namespace proto;

namespace {

void spell(char* buf, const char* prefix, long i) { std::snprintf(buf, 32, "%s-%010ld", prefix, i); }

double usSince(std::chrono::steady_clock::time_point t0, long n) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
}

void printIndexMemory() {
#ifdef PROTO_SYMBOL_TABLE_TRIE
    const SymbolTable::Shape s = globalSymbolTable().shape();
    std::printf("trie: leaves=%llu nodes=%llu chain_entries=%llu max_depth=%llu index_MB=%.1f\n",
                static_cast<unsigned long long>(s.leaves), static_cast<unsigned long long>(s.nodes),
                static_cast<unsigned long long>(s.chainEntries), static_cast<unsigned long long>(s.maxDepth),
                s.indexBytes / (1024.0 * 1024.0));
#endif
}

int fill(int argc, char** argv) {
    const long total = std::atol(argv[2]);
    std::vector<long> checkpoints;
    bool strict = false;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "strict")) strict = true;
        else checkpoints.push_back(std::atol(argv[i]));
    }
    std::sort(checkpoints.begin(), checkpoints.end());

    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const proto_ulong before = globalSymbolCount();
    constexpr long kProbe = 1000;
    std::vector<const ProtoString*> sym(total, nullptr);
    long probeBase = 0;
    long failures = 0;
    std::mt19937_64 rng(42);
    char buf[32];
    std::vector<double> fillRates;

    std::printf("table_size fill_us new_us lookup_us\n");
    long filled = 0;
    for (long cp : checkpoints) {
        if (cp > total) break;
        const long from = filled;
        const auto tf = std::chrono::steady_clock::now();
        for (; filled < cp; ++filled) {
            spell(buf, "sym", filled);
            sym[filled] = ProtoString::createSymbol(&ctx, buf);
        }
        const double fillUs = cp > from ? usSince(tf, cp - from) : 0.0;
        if (cp > from) fillRates.push_back(fillUs);
        std::vector<long> picks(kProbe);
        for (long& p : picks) p = static_cast<long>(rng() % static_cast<uint64_t>(filled));
        const auto tl = std::chrono::steady_clock::now();
        for (long p : picks) {
            spell(buf, "sym", p);
            if (ProtoString::createSymbol(&ctx, buf) != sym[p]) ++failures;
        }
        const double lookupUs = usSince(tl, kProbe);
        const auto tn = std::chrono::steady_clock::now();
        for (long i = 0; i < kProbe; ++i) {
            spell(buf, "probe", probeBase + i);
            if (!ProtoString::createSymbol(&ctx, buf)) ++failures;
        }
        const double newUs = usSince(tn, kProbe);
        probeBase += kProbe;
        std::printf("%ld %.3f %.3f %.3f\n", cp, fillUs, newUs, lookupUs);
        std::fflush(stdout);
    }
    for (; filled < total; ++filled) {
        spell(buf, "sym", filled);
        sym[filled] = ProtoString::createSymbol(&ctx, buf);
    }

    std::unordered_set<const ProtoString*> distinct(sym.begin(), sym.end());
    if (static_cast<long>(distinct.size()) != total) ++failures;
    for (long i = 0; i < total; ++i) {
        spell(buf, "sym", i);
        if (ProtoString::createSymbol(&ctx, buf) != sym[i]) ++failures;
    }
    const proto_ulong grew = globalSymbolCount() - before;
    const bool verified = failures == 0 && grew == static_cast<proto_ulong>(total + probeBase);
    bool linear = true;
    double ratio = 0.0;
    if (fillRates.size() >= 3) {
        ratio = fillRates.back() / fillRates[1];
        linear = ratio <= 3.0;
    }
    printIndexMemory();
    std::printf("interned=%ld distinct=%zu table_grew=%llu expected=%ld failures=%ld verified=%s "
                "fill_ratio_last_vs_second=%.2f linear=%s\n",
                total, distinct.size(), static_cast<unsigned long long>(grew), total + probeBase,
                failures, verified ? "yes" : "NO", ratio, linear ? "yes" : "NO");
    return verified && (linear || !strict) ? 0 : 1;
}

// ---- readers ----------------------------------------------------------------

struct ReaderRun {
    long tableSize = 0;
    const std::vector<const ProtoString*>* sym = nullptr;
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long> lookups{0};
    std::atomic<long> wrong{0};
    std::atomic<long> inserts{0};
    std::atomic<int> nextId{0};
    std::atomic<long> freshBase{0};
};
ReaderRun* g_run = nullptr;

const ProtoObject* readerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoArguments*, const ProtoKeywordArguments*) {
    ReaderRun& run = *g_run;
    std::mt19937_64 rng(1000 + run.nextId.fetch_add(1));
    char buf[32];
    long n = 0, wrong = 0;
    while (!run.go.load(std::memory_order_acquire)) ProtoThread::sleepFor(ctx, 100);
    while (!run.stop.load(std::memory_order_relaxed)) {
        for (int k = 0; k < 256; ++k) {
            const long p = static_cast<long>(rng() % static_cast<uint64_t>(run.tableSize));
            spell(buf, "sym", p);
            if (ProtoString::createSymbol(ctx, buf) != (*run.sym)[p]) ++wrong;
        }
        n += 256;
        ctx->safepoint();
    }
    run.lookups.fetch_add(n);
    run.wrong.fetch_add(wrong);
    return PROTO_NONE;
}

const ProtoObject* writerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoArguments*, const ProtoKeywordArguments*) {
    ReaderRun& run = *g_run;
    char buf[32];
    long n = 0;
    while (!run.go.load(std::memory_order_acquire)) ProtoThread::sleepFor(ctx, 100);
    while (!run.stop.load(std::memory_order_relaxed)) {
        const long base = run.freshBase.fetch_add(64);
        for (long i = 0; i < 64; ++i) {
            spell(buf, "fresh", base + i);
            (void) ProtoString::createSymbol(ctx, buf);
        }
        n += 64;
        ctx->safepoint();
    }
    run.inserts.fetch_add(n);
    return PROTO_NONE;
}

const ProtoString* shortName(ProtoContext* ctx, const char* prefix, int i) {
    return ProtoString::createSymbol(ctx, (std::string(prefix) + std::to_string(i % 1000)).c_str());
}

int readers(int argc, char** argv) {
    if (argc < 6) return 2;
    const long tableSize = std::atol(argv[2]);
    const double seconds = std::atof(argv[3]);
    const int writers = std::atoi(argv[4]);
    std::vector<int> counts;
    for (char* tok = std::strtok(argv[5], ","); tok; tok = std::strtok(nullptr, ",")) counts.push_back(std::atoi(tok));

    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    std::vector<const ProtoString*> sym(tableSize);
    char buf[32];
    for (long i = 0; i < tableSize; ++i) {
        spell(buf, "sym", i);
        sym[i] = ProtoString::createSymbol(&ctx, buf);
    }
    std::printf("readers table=%ld writers=%d seconds=%.1f\n", tableSize, writers, seconds);
    std::printf("readers lookups_per_s lookups_per_s_per_reader writer_interns_per_s verified\n");
    long freshBase = 0;
    bool ok = true;
    for (int r : counts) {
        ReaderRun run;
        run.tableSize = tableSize;
        run.sym = &sym;
        run.freshBase = freshBase;
        g_run = &run;
        std::vector<const ProtoThread*> threads;
        for (int i = 0; i < r; ++i)
            threads.push_back(space.newThread(&ctx, shortName(&ctx, "r", i), readerMain, nullptr, nullptr));
        for (int i = 0; i < writers; ++i)
            threads.push_back(space.newThread(&ctx, shortName(&ctx, "w", i), writerMain, nullptr, nullptr));
        const auto t0 = std::chrono::steady_clock::now();
        run.go.store(true, std::memory_order_release);
        {
            ProtoContext::UnmanagedScope out(&ctx);
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {}
        }
        run.stop.store(true);
        for (const ProtoThread* t : threads) const_cast<ProtoThread*>(t)->join(&ctx);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        freshBase = run.freshBase.load();
        const bool verified = run.wrong.load() == 0 && run.lookups.load() > 0;
        ok = ok && verified;
        std::printf("%d %.0f %.0f %.0f %s\n", r, run.lookups.load() / elapsed,
                    run.lookups.load() / elapsed / r, run.inserts.load() / elapsed,
                    verified ? "yes" : "NO");
        std::fflush(stdout);
        g_run = nullptr;
    }
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && !std::strcmp(argv[1], "fill")) return fill(argc, argv);
    if (argc >= 6 && !std::strcmp(argv[1], "readers")) return readers(argc, argv);
    std::fprintf(stderr,
                 "usage: %s fill <total> <checkpoint> [<checkpoint> ...] [strict]\n"
                 "       %s readers <tableSize> <seconds> <writers> <r1,r2,...>\n",
                 argv[0], argv[0]);
    return 2;
}
