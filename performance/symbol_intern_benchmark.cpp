/*
 * symbol_intern_benchmark.cpp -- the cost of interning a symbol as the
 * process-wide symbol table grows.
 *
 *   symbol_intern_benchmark <total> <checkpoint> [<checkpoint> ...]
 *
 * Interns `total` distinct spellings (14 bytes each, never the inline form)
 * through ProtoString::createSymbol.  At each checkpoint -- a table holding
 * that many of them -- it reports:
 *
 *   fill_us_per_symbol  mean cost of each intern since the previous checkpoint
 *                       (constant when the fill is linear)
 *   new_us              mean cost of interning 1,000 new spellings
 *   lookup_us           mean cost of createSymbol on 1,000 spellings already
 *                       interned (spread over the whole table)
 *
 * Self-verifying, exit status 1 on any failure: createSymbol of an interned
 * spelling returns the pointer it returned the first time (all of them, at
 * the end); no two spellings share a pointer; the table grew by exactly the
 * number of spellings interned.
 */

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <unordered_set>
#include <vector>

using namespace proto;

namespace {
void spell(char* buf, long i) { std::snprintf(buf, 32, "sym-%010ld", i); }

double usSince(std::chrono::steady_clock::time_point t0, long n) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <total> <checkpoint> [<checkpoint> ...]\n", argv[0]);
        return 2;
    }
    const long total = std::atol(argv[1]);
    std::vector<long> checkpoints;
    for (int i = 2; i < argc; ++i) checkpoints.push_back(std::atol(argv[i]));
    std::sort(checkpoints.begin(), checkpoints.end());

    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const proto_ulong before = globalSymbolCount();
    constexpr long kProbe = 1000;
    // Spellings [0, total) are the fill; [total, total + kProbe * checkpoints)
    // are the new-intern probes, interned at their checkpoint.
    std::vector<const ProtoString*> sym(total, nullptr);
    long probeBase = total;
    long probesInterned = 0;
    long failures = 0;
    std::mt19937_64 rng(42);
    char buf[32];

    std::printf("table_size fill_us_per_symbol new_us lookup_us\n");
    long filled = 0;
    for (long cp : checkpoints) {
        if (cp > total) break;
        const long from = filled;
        const auto tf = std::chrono::steady_clock::now();
        for (; filled < cp; ++filled) {
            spell(buf, filled);
            sym[filled] = ProtoString::createSymbol(&ctx, buf);
        }
        const double fillUs = cp > from ? usSince(tf, cp - from) : 0.0;
        // Lookups: existing spellings drawn from the whole filled range.
        std::vector<long> picks(kProbe);
        for (long& p : picks) p = static_cast<long>(rng() % static_cast<uint64_t>(filled));
        const auto tl = std::chrono::steady_clock::now();
        for (long p : picks) {
            spell(buf, p);
            if (ProtoString::createSymbol(&ctx, buf) != sym[p]) ++failures;
        }
        const double lookupUs = usSince(tl, kProbe);
        // New interns.
        const auto tn = std::chrono::steady_clock::now();
        for (long i = 0; i < kProbe; ++i) {
            spell(buf, probeBase + i);
            if (!ProtoString::createSymbol(&ctx, buf)) ++failures;
        }
        const double newUs = usSince(tn, kProbe);
        probeBase += kProbe;
        probesInterned += kProbe;
        std::printf("%ld %.3f %.3f %.3f\n", cp, fillUs, newUs, lookupUs);
        std::fflush(stdout);
    }
    for (; filled < total; ++filled) {
        spell(buf, filled);
        sym[filled] = ProtoString::createSymbol(&ctx, buf);
    }

    // Verification.
    std::unordered_set<const ProtoString*> distinct(sym.begin(), sym.end());
    if (static_cast<long>(distinct.size()) != total) ++failures;
    for (long i = 0; i < total; ++i) {
        spell(buf, i);
        if (ProtoString::createSymbol(&ctx, buf) != sym[i]) ++failures;
    }
    const proto_ulong grew = globalSymbolCount() - before;
    const bool ok = failures == 0 && grew == static_cast<proto_ulong>(total + probesInterned);
    std::printf("interned=%ld distinct=%zu table_grew=%llu expected=%ld failures=%ld verified=%s\n",
                total, distinct.size(), static_cast<unsigned long long>(grew), total + probesInterned,
                failures, ok ? "yes" : "NO");
    return ok ? 0 : 1;
}
