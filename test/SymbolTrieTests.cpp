// SymbolTrieTests.cpp -- the lock-free, write-once symbol trie
// (core/SymbolTable.cpp).
//
// The process-wide table is shared by every test of this process, so tests
// that force degenerate hashes build private tables (SymbolTable(mix)).  A
// private table is never destroyed here: the symbols it publishes are
// perennial, and the table is what keeps them reachable for LeakSanitizer.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

using namespace proto;

namespace {

SymbolTable* keptTable(SymbolTable::HashMix mix) {
    static auto* kept = new std::vector<SymbolTable*>();
    kept->push_back(new SymbolTable(mix));
    return kept->back();
}

const ProtoObject* internBytes(SymbolTable& t, ProtoContext* ctx, const std::string& s) {
    return t.internUTF8(ctx, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
const ProtoObject* lookupBytes(const SymbolTable& t, ProtoContext* ctx, const std::string& s) {
    return t.lookupUTF8(ctx, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
// A thread name of at most six bytes: an inline string, never in the table,
// so it does not disturb the table's counts.
const ProtoString* shortName(ProtoContext* ctx, const char* prefix, int i) {
    return ProtoString::createSymbol(ctx, (std::string(prefix) + std::to_string(i)).c_str());
}

std::string spelling(const char* prefix, long i) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s-%09ld", prefix, i);
    return buf;
}

// Every spelling interns to one stable, distinct pointer that both lookups
// find, and the table counts each once.
void expectCanonical(SymbolTable& t, ProtoContext* ctx, const char* prefix, long n) {
    std::vector<const ProtoObject*> sym(n);
    for (long i = 0; i < n; ++i) sym[i] = internBytes(t, ctx, spelling(prefix, i));
    long unstable = 0, missing = 0;
    for (long i = 0; i < n; ++i) {
        if (internBytes(t, ctx, spelling(prefix, i)) != sym[i]) ++unstable;
        if (lookupBytes(t, ctx, spelling(prefix, i)) != sym[i]) ++missing;
    }
    const std::unordered_set<const ProtoObject*> distinct(sym.begin(), sym.end());
    EXPECT_EQ(unstable, 0) << "a spelling interned twice gave two pointers";
    EXPECT_EQ(missing, 0) << "lookupUTF8 missed an interned spelling";
    EXPECT_EQ(distinct.size(), static_cast<std::size_t>(n)) << "two spellings share a symbol";
    EXPECT_EQ(lookupBytes(t, ctx, spelling(prefix, n)), nullptr) << "a spelling never interned was found";
}

}  // namespace

// The default mixer spreads 100,000 spellings over the root and a few levels.
TEST(SymbolTrie, DefaultHashKeepsTheTrieShallow) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    SymbolTable& t = *keptTable(nullptr);
    constexpr long n = 100000;
    expectCanonical(t, &ctx, "shallow", n);
    const SymbolTable::Shape s = t.shape();
    std::printf("[ SymbolTrie ] default: leaves=%llu nodes=%llu depth=%llu chains=%llu index=%zu bytes\n",
                (unsigned long long) s.leaves, (unsigned long long) s.nodes,
                (unsigned long long) s.maxDepth, (unsigned long long) s.chainEntries, s.indexBytes);
    EXPECT_EQ(s.leaves, static_cast<proto_ulong>(n));
    EXPECT_EQ(t.entryCount(), static_cast<proto_ulong>(n));
    EXPECT_EQ(s.chainEntries, 0u);
    EXPECT_LE(s.maxDepth, 4u);
}

// Every hash agrees on all bits but the top four: each insertion descends
// through a node at every level to the last one, which the top bits index.
TEST(SymbolTrie, HashesSharingAllButTheTopBitsReachTheLastLevel) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    SymbolTable& t = *keptTable([](uint64_t h) -> uint64_t { return (h & 0xF) << 60; });
    constexpr long n = 16;   // at most 16 distinct hashes ...
    expectCanonical(t, &ctx, "deep", n);
    const SymbolTable::Shape s = t.shape();
    std::printf("[ SymbolTrie ] deep: leaves=%llu nodes=%llu depth=%llu chains=%llu\n",
                (unsigned long long) s.leaves, (unsigned long long) s.nodes,
                (unsigned long long) s.maxDepth, (unsigned long long) s.chainEntries);
    EXPECT_EQ(s.leaves, static_cast<proto_ulong>(n));
    EXPECT_EQ(s.maxDepth, static_cast<proto_ulong>(SymbolTable::kMaxLevel));
    EXPECT_EQ(s.nodes, static_cast<proto_ulong>(SymbolTable::kMaxLevel)) << "one node per level below the root";
}

// One hash for every spelling: the root slot becomes a full-collision chain.
TEST(SymbolTrie, IdenticalHashesShareAWriteOnceChain) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    SymbolTable& t = *keptTable([](uint64_t) -> uint64_t { return 0x1234; });
    constexpr long n = 200;
    expectCanonical(t, &ctx, "collide", n);
    const SymbolTable::Shape s = t.shape();
    EXPECT_EQ(s.leaves, static_cast<proto_ulong>(n));
    EXPECT_EQ(s.chainEntries, static_cast<proto_ulong>(n));
    EXPECT_EQ(s.nodes, 0u);
}

// Two hashes that differ only in the last bit, each shared by many spellings:
// two chains that only the last level separates, so the first chain is pushed
// down through every level when the second hash arrives.
TEST(SymbolTrie, CollisionChainsArePushedDownByADifferentHash) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    SymbolTable& t = *keptTable([](uint64_t h) -> uint64_t { return (h & 1) << 63; });
    constexpr long n = 300;
    expectCanonical(t, &ctx, "pushdown", n);
    const SymbolTable::Shape s = t.shape();
    std::printf("[ SymbolTrie ] pushdown: leaves=%llu nodes=%llu depth=%llu chains=%llu\n",
                (unsigned long long) s.leaves, (unsigned long long) s.nodes,
                (unsigned long long) s.maxDepth, (unsigned long long) s.chainEntries);
    EXPECT_EQ(s.leaves, static_cast<proto_ulong>(n));
    EXPECT_EQ(s.chainEntries, static_cast<proto_ulong>(n));
    EXPECT_EQ(s.maxDepth, static_cast<proto_ulong>(SymbolTable::kMaxLevel));
}

// A spelling reaches one symbol whether it arrives as UTF-8 bytes or as a
// heap string whose rope splits it at different leaf boundaries.
TEST(SymbolTrie, BytesAndRopesOfOneSpellingGiveOneSymbol) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 2);
    const std::string text = "a spelling long enough to span several rope leaves - "
                             "\xC3\xA1\xC3\xA9\xC3\xAD\xC3\xB3\xC3\xBA - and then some more bytes";
    // A rope built by concatenating pieces: its leaves split the text where
    // the pieces end, not where a fresh build would.
    const ProtoString* rope = ProtoString::fromUTF8(&ctx, text.substr(0, 7).c_str());
    for (std::size_t at = 7; at < text.size();) {
        std::size_t len = std::min<std::size_t>(11, text.size() - at);
        while (at + len < text.size() && (static_cast<unsigned char>(text[at + len]) & 0xC0) == 0x80) ++len;
        rope = rope->appendLast(&ctx, ProtoString::fromUTF8(&ctx, text.substr(at, len).c_str()));
        at += len;
    }
    ctx.getAutomaticLocals()[0] = rope->asObject(&ctx);
    SymbolTable& global = globalSymbolTable();
    EXPECT_EQ(global.lookupByContent(&ctx, rope->asObject(&ctx)), nullptr) << "spelling interned already";
    const ProtoObject* fromRope = global.intern(&ctx, rope->asObject(&ctx));
    const ProtoObject* fromBytes = internBytes(global, &ctx, text);
    const ProtoObject* viaApi = ProtoString::createSymbol(&ctx, text.c_str())->asObject(&ctx);
    EXPECT_TRUE(SymbolTable::isSymbol(fromRope));
    EXPECT_EQ(fromRope, fromBytes);
    EXPECT_EQ(fromRope, viaApi);
    EXPECT_EQ(global.lookupByContent(&ctx, rope->asObject(&ctx)), fromRope);
    EXPECT_EQ(lookupBytes(global, &ctx, text), fromRope);
    // A spelling that differs in its last byte is a different symbol.
    std::string other = text;
    other.back() = '!';
    EXPECT_NE(internBytes(global, &ctx, other), fromRope);
}

namespace {

constexpr int kInterners = 8;
constexpr long kShared = 30000;
std::vector<const ProtoObject*> g_seen[kInterners];
std::atomic<int> g_nextInterner{0};

const ProtoObject* internerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                const ProtoArguments*, const ProtoKeywordArguments*) {
    const int id = g_nextInterner.fetch_add(1);
    std::vector<const ProtoObject*>& seen = g_seen[id];
    seen.assign(kShared, nullptr);
    for (long k = 0; k < kShared; ++k) {
        // Each thread starts at a different point and half of them walk
        // backwards, so they race to publish the same spellings.
        long i = (k + id * (kShared / kInterners)) % kShared;
        if (id & 1) i = kShared - 1 - i;
        seen[i] = ProtoString::createSymbol(ctx, spelling("race", i).c_str())->asObject(ctx);
    }
    return PROTO_NONE;
}

constexpr int kReaders = 4;
constexpr long kPublished = 20000;    // interned before the readers start
constexpr long kFresh = 20000;        // interned by the writers while they read
std::atomic<bool> g_writersDone{false};
std::atomic<long> g_readerFaults{0};
std::atomic<long> g_freshSeen{0};
const ProtoObject* g_published[kPublished];

const ProtoObject* readerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoArguments*, const ProtoKeywordArguments*) {
    SymbolTable& t = globalSymbolTable();
    long round = 0;
    do {
        for (long i = round % 7; i < kPublished; i += 7)
            if (lookupBytes(t, ctx, spelling("pub", i)) != g_published[i]) g_readerFaults.fetch_add(1);
        for (long i = round % 5; i < kFresh; i += 5) {
            const ProtoObject* s = lookupBytes(t, ctx, spelling("fresh", i));
            if (s) {
                g_freshSeen.fetch_add(1, std::memory_order_relaxed);
                // A symbol a reader sees is complete and spells its name.
                if (!SymbolTable::isSymbol(s) ||
                    reinterpret_cast<const ProtoString*>(s)->getSize(ctx) != spelling("fresh", i).size())
                    g_readerFaults.fetch_add(1);
            }
        }
        ++round;
    } while (!g_writersDone.load());
    return PROTO_NONE;
}

const ProtoObject* writerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoArguments* args, const ProtoKeywordArguments*) {
    static std::atomic<int> nextWriter{0};
    const int w = nextWriter.fetch_add(1);
    for (long i = w; i < kFresh; i += 2) (void) ProtoString::createSymbol(ctx, spelling("fresh", i).c_str());
    return PROTO_NONE;
}

}  // namespace

// Eight ProtoThreads intern the same 30,000 new spellings in different orders
// and agree on every pointer; the table grows by exactly 30,000.
TEST(SymbolTrie, ConcurrentInternersAgreeOnEveryPointer) {
    ProtoSpace space;
    ProtoContext caller(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const proto_ulong before = globalSymbolCount();
    std::vector<const ProtoThread*> threads;
    for (int t = 0; t < kInterners; ++t)
        threads.push_back(space.newThread(&caller, shortName(&caller, "ti", t), internerMain, nullptr, nullptr));
    for (const ProtoThread* t : threads) const_cast<ProtoThread*>(t)->join(&caller);
    long disagreements = 0, missing = 0;
    for (long i = 0; i < kShared; ++i)
        for (int t = 0; t < kInterners; ++t) {
            if (!g_seen[t][i]) ++missing;
            else if (g_seen[t][i] != g_seen[0][i]) ++disagreements;
        }
    const proto_ulong grew = globalSymbolCount() - before;
    std::printf("[ SymbolTrie ] concurrent interners: disagreements=%ld missing=%ld grew=%llu\n",
                disagreements, missing, (unsigned long long) grew);
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(disagreements, 0);
    EXPECT_EQ(grew, static_cast<proto_ulong>(kShared));
}

// Four readers look symbols up without pause while two writers intern new
// ones: published symbols are always found, and a symbol being published is
// either absent or complete (the ThreadSanitizer job checks the ordering).
TEST(SymbolTrie, LookupsRunConcurrentlyWithInserts) {
    ProtoSpace space;
    ProtoContext caller(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    for (long i = 0; i < kPublished; ++i)
        g_published[i] = ProtoString::createSymbol(&caller, spelling("pub", i).c_str())->asObject(&caller);
    g_writersDone = false;
    std::vector<const ProtoThread*> readers, writers;
    for (int r = 0; r < kReaders; ++r)
        readers.push_back(space.newThread(&caller, shortName(&caller, "tr", r), readerMain, nullptr, nullptr));
    for (int w = 0; w < 2; ++w)
        writers.push_back(space.newThread(&caller, shortName(&caller, "tw", w), writerMain, nullptr, nullptr));
    for (const ProtoThread* t : writers) const_cast<ProtoThread*>(t)->join(&caller);
    g_writersDone = true;
    for (const ProtoThread* t : readers) const_cast<ProtoThread*>(t)->join(&caller);
    long missingFresh = 0;
    for (long i = 0; i < kFresh; ++i)
        if (!lookupBytes(globalSymbolTable(), &caller, spelling("fresh", i))) ++missingFresh;
    std::printf("[ SymbolTrie ] concurrent lookups: faults=%ld fresh_seen_by_readers=%ld missing_after=%ld\n",
                g_readerFaults.load(), g_freshSeen.load(), missingFresh);
    EXPECT_EQ(g_readerFaults.load(), 0);
    EXPECT_EQ(missingFresh, 0);
}

// A heap-string attribute name is counted; a symbol name is not.
TEST(SymbolTrie, NonInternedAttributeNamesAreCounted) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr, 2);
    const ProtoObject* obj = ctx.newObject(true);
    const ProtoString* sym = ProtoString::createSymbol(&ctx, "counted-attribute-name");
    const ProtoString* str = ProtoString::fromUTF8(&ctx, "counted-attribute-name");
    ctx.getAutomaticLocals()[0] = obj;
    ctx.getAutomaticLocals()[1] = str->asObject(&ctx);
    const NonInternedNameCounts before = ProtoSpace::nonInternedNameCounts();
    obj = obj->setAttribute(&ctx, sym, ctx.fromInteger(1));
    (void) obj->getAttribute(&ctx, sym);
    (void) obj->hasAttribute(&ctx, sym);
    const NonInternedNameCounts afterSymbols = ProtoSpace::nonInternedNameCounts();
    obj = obj->setAttribute(&ctx, str, ctx.fromInteger(2));
    (void) obj->getAttribute(&ctx, str);
    (void) obj->getAttribute(&ctx, str);
    (void) obj->hasAttribute(&ctx, str);
    (void) obj->hasOwnAttribute(&ctx, str);
    const NonInternedNameCounts after = ProtoSpace::nonInternedNameCounts();
    EXPECT_EQ(afterSymbols.total(), before.total()) << "a symbol name was counted";
    EXPECT_EQ(after.set - before.set, 1u);
    EXPECT_EQ(after.get - before.get, 2u);
    EXPECT_EQ(after.has - before.has, 1u);
    EXPECT_EQ(after.hasOwn - before.hasOwn, 1u);
    EXPECT_EQ(obj->getAttribute(&ctx, sym)->asLong(&ctx), 2) << "the heap-string name did not reach the symbol's slot";
}
