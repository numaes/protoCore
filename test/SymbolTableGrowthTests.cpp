// SymbolTableGrowthTests.cpp -- the process-wide symbol table keeps its chains
// short as it grows, and growth is safe under concurrent interning.
//
// Until 3.3.x each of the table's 64 shards was one linked chain: with a
// million symbols a lookup walked ~15,600 nodes, and filling the table was
// quadratic (measured by the protoJS agent: ~22 min for 1M symbols).  Each
// shard is now a chained hash table that doubles its bucket array at load
// factor 1 (SymbolTable::Shard::insert).  These tests check the structure
// rather than time: chain length is what made the cost grow.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

using namespace proto;

namespace {

struct ChainStats {
    std::size_t longest = 0;
    std::size_t entries = 0;
    std::size_t maxLoadNumerator = 0;   // shard entries
    std::size_t maxLoadDenominator = 1; // shard buckets
};

ChainStats chainStats() {
    ChainStats st;
    SymbolTable& table = globalSymbolTable();
    for (auto& shard : table.shards) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        st.entries += shard.count;
        if (!shard.buckets.empty() &&
            shard.count * st.maxLoadDenominator > st.maxLoadNumerator * shard.buckets.size()) {
            st.maxLoadNumerator = shard.count;
            st.maxLoadDenominator = shard.buckets.size();
        }
        for (const SymbolTable::Bucket* head : shard.buckets) {
            std::size_t n = 0;
            for (const SymbolTable::Bucket* b = head; b; b = b->next) ++n;
            if (n > st.longest) st.longest = n;
        }
    }
    return st;
}

}  // namespace

// 200,000 spellings: every one interns to a distinct, stable pointer, the
// table grows by exactly that many entries, no shard holds more entries than
// buckets, and no chain is longer than a handful of nodes (the single-chain
// table had ~3,000 per shard at this size).
TEST(SymbolTableGrowth, ChainsStayShortAsTheTableGrows) {
    constexpr long kSpellings = 200000;
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const proto_ulong before = globalSymbolCount();
    std::vector<const ProtoString*> sym(kSpellings);
    char buf[32];
    for (long i = 0; i < kSpellings; ++i) {
        std::snprintf(buf, sizeof buf, "growth-%09ld", i);
        sym[i] = ProtoString::createSymbol(&ctx, buf);
    }
    long changed = 0;
    for (long i = 0; i < kSpellings; ++i) {
        std::snprintf(buf, sizeof buf, "growth-%09ld", i);
        if (ProtoString::createSymbol(&ctx, buf) != sym[i]) ++changed;
    }
    const std::unordered_set<const ProtoString*> distinct(sym.begin(), sym.end());
    const ChainStats st = chainStats();
    std::printf("[ SymbolTableGrowth ] entries=%zu longest_chain=%zu max_shard_load=%zu/%zu\n",
                st.entries, st.longest, st.maxLoadNumerator, st.maxLoadDenominator);
    EXPECT_EQ(changed, 0) << "an interned spelling changed pointer";
    EXPECT_EQ(distinct.size(), static_cast<std::size_t>(kSpellings)) << "two spellings share a symbol";
    EXPECT_EQ(globalSymbolCount() - before, static_cast<proto_ulong>(kSpellings));
    EXPECT_LE(st.maxLoadNumerator, st.maxLoadDenominator) << "a shard holds more entries than buckets";
    EXPECT_LE(st.longest, 16u) << "a chain grew long: the bucket array did not grow";
}

namespace {

constexpr int kInterners = 8;
constexpr long kShared = 40000;   // every thread interns the same spellings
std::vector<const ProtoString*> g_seen[kInterners];

const ProtoObject* internerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                const ProtoArguments* args, const ProtoKeywordArguments*) {
    static std::atomic<int> nextId{0};
    const int id = nextId.fetch_add(1) % kInterners;
    std::vector<const ProtoString*>& seen = g_seen[id];
    seen.assign(kShared, nullptr);
    char buf[32];
    // Each thread walks the spellings from a different starting point, so the
    // threads race to publish the same spellings while shards are growing.
    for (long k = 0; k < kShared; ++k) {
        const long i = (k + id * (kShared / kInterners)) % kShared;
        std::snprintf(buf, sizeof buf, "race-%09ld", i);
        seen[i] = ProtoString::createSymbol(ctx, buf);
    }
    return PROTO_NONE;
}

}  // namespace

// Eight ProtoThreads intern the same 40,000 new spellings at once, in
// different orders, while the shards grow: every thread gets the same
// pointer for each spelling, and the table grows by exactly 40,000.
TEST(SymbolTableGrowth, ConcurrentInternersAgreeWhileShardsGrow) {
    ProtoSpace space;
    ProtoContext caller(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const proto_ulong before = globalSymbolCount();
    std::vector<const ProtoThread*> threads;
    for (int t = 0; t < kInterners; ++t)
        threads.push_back(space.newThread(&caller,
            ProtoString::createSymbol(&caller, ("interner-" + std::to_string(t)).c_str()),
            internerMain, nullptr, nullptr));
    for (const ProtoThread* t : threads) const_cast<ProtoThread*>(t)->join(&caller);
    long disagreements = 0, missing = 0;
    for (long i = 0; i < kShared; ++i) {
        for (int t = 0; t < kInterners; ++t) {
            if (!g_seen[t][i]) ++missing;
            else if (g_seen[t][i] != g_seen[0][i]) ++disagreements;
        }
    }
    // +kInterners: the threads' own names, interned above.
    const proto_ulong grew = globalSymbolCount() - before;
    std::printf("[ SymbolTableGrowth ] concurrent: disagreements=%ld missing=%ld grew=%llu\n",
                disagreements, missing, static_cast<unsigned long long>(grew));
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(disagreements, 0) << "two threads got different symbols for one spelling";
    EXPECT_EQ(grew, static_cast<proto_ulong>(kShared + kInterners));
}
