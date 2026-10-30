/*
 * SymbolTable.cpp — the process-wide 64-shard concurrent string interning table.
 *
 * P3 (2026-09-24): ONE table per PROCESS, reached through globalSymbolTable().
 * ProtoSpace::symbolTable is a BORROWED pointer to it and ~ProtoSpace must NOT
 * free it.  Note that ~SymbolTable frees only the Bucket nodes, never the symbol
 * cells (they are perennial) — so before P3 every destroyed space leaked its
 * whole symbol set.  One table leaks one set.
 *
 * Each shard is an independent chained hash table protected by its own
 * mutex, eliminating the single-mutex bottleneck of TupleDictionary for
 * string lookups.  A shard's bucket array doubles when the shard holds as many
 * entries as buckets (Shard::insert), so chains stay O(1) long at any size.  A shard mutex is a STRICT LEAF LOCK: it is never ordered
 * against ProtoSpace::globalMutex, and no Cell is allocated while it is held.
 * With one table serving several spaces, breaking that invariant would deadlock
 * two collectors rather than one.
 *
 * Every interned string (symbol) is PERENNIAL. `intern` builds the canonical
 * rope with a NULL ProtoContext, which routes every Cell
 * allocation through `posix_memalign` directly: the Cells are never enrolled
 * in any thread freelist or context young-generation chain, so the GC's
 * mark/sweep and root collection never see them — they live for the lifetime
 * of the process. There is no "weak"/collectible symbol variant: a name, once
 * interned, must keep its canonical pointer forever for pointer-identity
 * symbol comparison to be sound. See DESIGN.md § "Perpetual allocations via
 * NULL ProtoContext".
 */

#include "proto_internal.h"
#include <cstring>
#include <mutex>
#include <vector>

namespace proto {

// ---------------------------------------------------------------------------
// globalSymbolTable — the one table of this process.
//
// Function-local static: initialisation is thread-safe since C++11, and the
// pointer is leaked on purpose so the table is never destroyed (see the
// declaration in proto_internal.h).
// ---------------------------------------------------------------------------
SymbolTable& globalSymbolTable() {
    static SymbolTable* table = new SymbolTable();
    return *table;
}

proto_ulong SymbolTable::entryCount() const {
    proto_ulong total = 0;
    for (int i = 0; i < SHARD_COUNT; ++i) {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(shards[i].mutex));
        total += shards[i].count;
    }
    return total;
}

proto_ulong globalSymbolCount() { return globalSymbolTable().entryCount(); }

// ---------------------------------------------------------------------------
// Destructor — free all bucket chains
// ---------------------------------------------------------------------------
SymbolTable::~SymbolTable() {
    for (int i = 0; i < SHARD_COUNT; ++i) {
        for (Bucket* b : shards[i].buckets) {
            while (b) {
                Bucket* next = b->next;
                delete b;
                b = next;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// isSymbol — true if obj carries POINTER_TAG_SYMBOL
// ---------------------------------------------------------------------------
bool SymbolTable::isSymbol(const ProtoObject* obj) {
    if (!obj) return false;
    ProtoObjectPointer pa{};
    pa.oid = obj;
    return pa.op.pointer_tag == POINTER_TAG_SYMBOL;
}

// ---------------------------------------------------------------------------
// contentEqual — byte-level UTF-8 equality between two ProtoString objects.
// Works for POINTER_TAG_STRING, POINTER_TAG_SYMBOL, and inline strings.
// ---------------------------------------------------------------------------
bool SymbolTable::contentEqual(ProtoContext* ctx,
                                const ProtoObject* a,
                                const ProtoObject* b) {
    const ProtoString* sa = reinterpret_cast<const ProtoString*>(a);
    const ProtoString* sb = reinterpret_cast<const ProtoString*>(b);
    if (sa->getSize(ctx) != sb->getSize(ctx)) return false;
    std::string ua, ub;
    sa->toUTF8String(ctx, ua);
    sb->toUTF8String(ctx, ub);
    return ua == ub;
}

// ---------------------------------------------------------------------------
// normalizeForSymbol — copy any string representation into a fresh rope and
// return its root; the symbol handle points at that root.
//
// `readCtx` is used only for reading from `strObj` (iterating its rope). The
// new rope nodes are ALWAYS allocated with a
// null ProtoContext: that routes every Cell allocation through
// `posix_memalign` directly, so the resulting Cells are never enrolled in any
// thread freelist, never appear in a context's young-generation chain, and
// are therefore invisible to the GC's free-and-recycle machinery — they live
// for the lifetime of the process. Every symbol is perennial by construction;
// see the file header and DESIGN.md § "Perpetual allocations via NULL
// ProtoContext".
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::normalizeForSymbol(
        ProtoContext* readCtx, const ProtoObject* strObj) {
    std::string utf8;
    reinterpret_cast<const ProtoString*>(strObj)->toUTF8String(readCtx, utf8);
    return buildRopeFromUTF8(
        /*ctx=*/nullptr,
        reinterpret_cast<const uint8_t*>(utf8.data()),
        utf8.size());
}

// ---------------------------------------------------------------------------
// contentHashOfUTF8 — the content hash of a run of UTF-8 bytes.
//
// Must stay identical to computeContentHash (core/ProtoString.cpp), which is
// FNV-1a over the leaf bytes in order and is the hash of every heap string: a lookup keyed by bytes has to land in the same shard
// and match the same bucket as one keyed by a ProtoString.
// ---------------------------------------------------------------------------
static uint64_t contentHashOfUTF8(const uint8_t* bytes, size_t len) {
    uint64_t h = 14695981039346656037ULL;   // FNV offset basis
    for (size_t i = 0; i < len; ++i)
        h = (h ^ bytes[i]) * 1099511628211ULL;
    return h;
}

// ---------------------------------------------------------------------------
// lookupUTF8 — read-only lookup keyed by raw UTF-8 bytes; allocates nothing.
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::lookupUTF8(ProtoContext* ctx,
                                            const uint8_t* bytes, size_t len) const {
    const uint64_t hash = contentHashOfUTF8(bytes, len);
    const Shard& shard = shards[shardIndex(hash)];

    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(shard.mutex));
    for (const Bucket* b = shard.chainFor(hash); b; b = b->next) {
        if (b->content_hash != hash) continue;
        std::string candidate;
        reinterpret_cast<const ProtoString*>(b->symbol)->toUTF8String(ctx, candidate);
        if (candidate.size() == len && std::memcmp(candidate.data(), bytes, len) == 0)
            return b->symbol;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// intern — return the canonical symbol for strObj, inserting if absent.
//
// All potentially-allocating work (normalizeForSymbol) is
// performed BEFORE acquiring the shard lock, so the shard mutex is never held
// across an allocation. This keeps the shard lock a strict leaf lock — it is
// never ordered against the GC global lock — and the double-checked-locking
// re-check inside the critical section handles the race where two threads
// intern the same string concurrently.
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::intern(ProtoContext* ctx,
                                        const ProtoObject* strObj) {
    if (!strObj) return strObj;

    ProtoObjectPointer pa{};
    pa.oid = strObj;
    if (pa.op.pointer_tag == POINTER_TAG_SYMBOL) return strObj;
    // Embedded values (inline strings, small integers, etc.) cannot be
    // interned as symbols — return unchanged.
    if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) return strObj;
    // A spelling of at most INLINE_STRING_MAX_BYTES bytes is canonical as an
    // inline string, so the table never holds one.
    if (const ProtoObject* inl = inlineFormOfString(ctx, strObj)) return inl;
    // Not a string: nothing to intern.
    if (!heapStringRoot(strObj)) return strObj;

    // Already interned?  Look it up first: normalizeForSymbol below builds a
    // perennial copy of the string (Cells from posix_memalign that no cycle
    // ever reclaims), and for a spelling that is already in the table that
    // copy is pure leak — it is dropped on the re-check inside the lock.
    if (const ProtoObject* existing = lookupByContent(ctx, strObj)) return existing;

    // Normalize and create the symbol candidate BEFORE acquiring the lock.
    // normalizeForSymbol may allocate Cells; doing so without holding
    // shard.mutex prevents the deadlock described above.
    //
    // normalizeForSymbol always builds the interned string with a NULL
    // ProtoContext, so every symbol's Cells are perpetual: allocated via
    // posix_memalign, never on any thread freelist or context young chain,
    // invisible to the GC. A symbol's identity must outlive every individual
    // context and thread that references it (symbols are attribute names,
    // language keywords, protoCore-side cached literals), and being perpetual
    // also removes any per-GC-cycle cost of protecting symbol Cells. The
    // candidate that loses the insert race in insertCandidate is likewise
    // perpetual, and kept reachable there.
    const ProtoObject* root = normalizeForSymbol(ctx, strObj);
    return insertCandidate(ctx, computeContentHash(root),
                           heapStringFromRoot(root, /*symbol=*/true));
}

// ---------------------------------------------------------------------------
// insertCandidate — publish a pre-built perennial symbol, or adopt the one
// another thread published first.
//
// The re-check runs under the shard lock: another thread may have interned
// the same spelling while the caller built its candidate.  The losing
// candidate is perennial like every symbol string, and is kept reachable in
// a never-destroyed list (one string per lost race): retained memory must be
// reachable, or LeakSanitizer reports it at exit
// (GlobalInterning.ConcurrentInterningAcrossSpacesAgreesOnOnePointer did).
// The list has its own mutex, taken after the shard lock is released, so the
// shard lock stays a leaf lock.
// ---------------------------------------------------------------------------
namespace {
std::mutex& lostCandidatesMutex() {
    static auto* m = new std::mutex();
    return *m;
}
std::vector<const ProtoObject*>& lostCandidates() {   // guarded by lostCandidatesMutex
    static auto* v = new std::vector<const ProtoObject*>();
    return *v;
}
}  // namespace

// Links `bucket` into its chain; doubles the bucket array first when the
// shard holds as many entries as buckets (load factor 1), relinking every
// node.  The caller holds `mutex`.  Amortised O(1): an entry is relinked
// once per doubling.
void SymbolTable::Shard::insert(Bucket* bucket) {
    if (count >= buckets.size()) {
        const std::size_t size = buckets.empty() ? kInitialBuckets : buckets.size() * 2;
        std::vector<Bucket*> grown(size, nullptr);
        for (Bucket* b : buckets) {
            while (b) {
                Bucket* next = b->next;
                Bucket*& head = grown[(b->content_hash >> SHARD_BITS) & (size - 1)];
                b->next = head;
                head = b;
                b = next;
            }
        }
        buckets.swap(grown);
    }
    Bucket*& head = buckets[(bucket->content_hash >> SHARD_BITS) & (buckets.size() - 1)];
    bucket->next = head;
    head = bucket;
    ++count;
}

const ProtoObject* SymbolTable::insertCandidate(ProtoContext* ctx, uint64_t hash,
                                                 const ProtoObject* candidate) {
    Shard& shard = shards[shardIndex(hash)];
    const ProtoObject* winner = nullptr;
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (Bucket* b = shard.chainFor(hash); b; b = b->next) {
            if (b->content_hash == hash && contentEqual(ctx, b->symbol, candidate)) {
                winner = b->symbol;
                break;
            }
        }
        if (!winner) {
            shard.insert(new Bucket{ hash, candidate, nullptr });
            return candidate;
        }
    }
    std::lock_guard<std::mutex> lock(lostCandidatesMutex());
    lostCandidates().push_back(candidate);
    return winner;
}

// ---------------------------------------------------------------------------
// internUTF8 — intern a spelling given as UTF-8 bytes, building ONE perennial
// copy of it.
//
// ProtoString::createSymbol used to build a perennial ProtoString from the
// bytes and pass it to intern(), whose normalizeForSymbol built a second
// perennial copy and kept that one: every new symbol left its first copy
// unreferenced for the life of the process (LeakSanitizer reported it in
// every embedder at exit).  Here the copy built from the bytes is the
// candidate itself.
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::internUTF8(ProtoContext* ctx,
                                            const uint8_t* bytes, size_t len) {
    if (const ProtoObject* existing = lookupUTF8(ctx, bytes, len)) return existing;
    // Built with a NULL ProtoContext: perennial, invisible to the collector,
    // so no cycle can reclaim it before it is published (see intern()).
    const ProtoObject* root = buildRopeFromUTF8(/*ctx=*/nullptr, bytes, len);
    return insertCandidate(ctx, computeContentHash(root),
                           heapStringFromRoot(root, /*symbol=*/true));
}

// ---------------------------------------------------------------------------
// lookupByContent — read-only lookup; returns nullptr if not yet interned.
//
// Unlike intern(), this method never allocates and never modifies the table.
// It is safe to call on hot read paths (getAttribute, hasAttribute, etc.)
// because a string that has never been stored as an attribute key cannot
// possibly exist in the attribute sparse list, so returning nullptr is
// equivalent to "attribute not found".
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::lookupByContent(ProtoContext* ctx,
                                                  const ProtoObject* strObj) const {
    if (!strObj) return nullptr;
    ProtoObjectPointer pa{}; pa.oid = strObj;
    if (pa.op.pointer_tag == POINTER_TAG_SYMBOL) return strObj;
    if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) return strObj;
    if (const ProtoObject* inl = inlineFormOfString(ctx, strObj)) return inl;

    const ProtoObject* root = heapStringRoot(strObj);
    if (!root) return nullptr;
    uint64_t hash = computeContentHash(root);
    int shard_idx = shardIndex(hash);
    const Shard& shard = shards[shard_idx];

    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(shard.mutex));
    for (const Bucket* b = shard.chainFor(hash); b; b = b->next) {
        if (b->content_hash == hash &&
            contentEqual(ctx, b->symbol, strObj))
            return b->symbol;
    }
    return nullptr;
}

} // namespace proto
