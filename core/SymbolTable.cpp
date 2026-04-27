/*
 * SymbolTable.cpp — 64-shard concurrent string interning table.
 *
 * Each shard holds an independent linked-list bucket chain protected by its
 * own mutex, eliminating the single-mutex bottleneck of TupleDictionary for
 * string lookups.
 *
 * Strong symbols (created by ProtoString::createSymbol) are pinned as GC
 * roots and are never collected. Weak symbols can be removed during GC sweep
 * via removeWeak().
 */

#include "proto_internal.h"
#include <cstring>

namespace proto {

// ---------------------------------------------------------------------------
// Destructor — free all bucket chains
// ---------------------------------------------------------------------------
SymbolTable::~SymbolTable() {
    for (int i = 0; i < SHARD_COUNT; ++i) {
        Bucket* b = shards[i].head;
        while (b) {
            Bucket* next = b->next;
            delete b;
            b = next;
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
// normalizeForSymbol — convert any string representation to a fresh
// ProtoStringImplementation whose hash reflects its AVL content.
//
// `readCtx` is used only for reading from `strObj` (iterating its rope);
// `allocCtx` is used for the new ProtoStringImplementation and its AVL
// nodes.  The two are kept distinct so a strong intern can pass
// `allocCtx = nullptr`, which routes every Cell allocation through
// `posix_memalign` directly: the resulting Cells are never enrolled in
// any thread freelist, never appear in a context's young-generation
// chain, and are therefore invisible to the GC's free-and-recycle
// machinery — they live for the lifetime of the process.  See
// DESIGN.md § "Perpetual allocations via NULL ProtoContext".
// ---------------------------------------------------------------------------
const ProtoStringImplementation* SymbolTable::normalizeForSymbol(
        ProtoContext* readCtx, ProtoContext* allocCtx,
        const ProtoObject* strObj) {
    std::string utf8;
    reinterpret_cast<const ProtoString*>(strObj)->toUTF8String(readCtx, utf8);
    return ProtoStringImplementation::fromUTF8Bytes(
        allocCtx,
        reinterpret_cast<const uint8_t*>(utf8.data()),
        utf8.size());
}

// ---------------------------------------------------------------------------
// intern — return the canonical symbol for strObj, inserting if absent.
//
// Allocation (normalizeForSymbol / implAsSymbol) is performed BEFORE acquiring
// the shard lock to avoid a lock-order inversion with the GC global lock:
//
//   Thread A: holds shard.mutex → triggers allocation → waits for globalMutex
//   GC thread: holds globalMutex → calls removeWeak → waits for shard.mutex
//              → DEADLOCK
//
// The double-checked locking pattern below avoids this by performing all
// potentially-allocating work outside the critical section.  A re-check
// inside the lock handles the race where two threads intern the same string
// concurrently.
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::intern(ProtoContext* ctx,
                                        const ProtoObject* strObj,
                                        bool is_strong) {
    if (!strObj) return strObj;

    ProtoObjectPointer pa{};
    pa.oid = strObj;
    if (pa.op.pointer_tag == POINTER_TAG_SYMBOL) return strObj;
    // Embedded values (inline strings, small integers, etc.) cannot be
    // interned as symbols — return unchanged.
    if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) return strObj;

    // Normalize and create the symbol candidate BEFORE acquiring the lock.
    // normalizeForSymbol may allocate Cells; doing so without holding
    // shard.mutex prevents the deadlock described above.
    //
    // Strong symbols are allocated with `allocCtx = nullptr`, which makes
    // their Cells perpetual (posix_memalign-only, never on any thread
    // freelist or context young chain).  This is correct because a
    // strong symbol's identity must outlive every individual context
    // and thread that ever references it — typical strong symbols are
    // attribute names, language keywords, and protoCore-side cached
    // literals — and it removes the per-GC-cycle cost of having to
    // mark every strong-symbol Cell as a root.  Weak symbols continue
    // to allocate against `ctx` and follow the normal GC lifetime.
    ProtoContext* allocCtx = is_strong ? nullptr : ctx;
    const ProtoStringImplementation* normalized =
        normalizeForSymbol(ctx, allocCtx, strObj);
    const ProtoObject* symbol_candidate = reinterpret_cast<const ProtoObject*>(
        normalized->implAsSymbol(ctx));
    uint64_t hash = normalized->implGetHash();
    int shard_idx = shardIndex(hash);
    Shard& shard = shards[shard_idx];

    {
        std::lock_guard<std::mutex> lock(shard.mutex);

        // Re-check: another thread may have interned the same string while
        // we were normalizing above.  If so, return the existing symbol and
        // let the GC collect symbol_candidate.
        for (Bucket* b = shard.head; b; b = b->next) {
            if (b->content_hash == hash &&
                contentEqual(ctx, b->symbol, strObj))
                return b->symbol;
        }

        // Not found — insert the pre-built candidate.
        Bucket* bucket = new Bucket{ hash, symbol_candidate, is_strong, shard.head };
        shard.head = bucket;
        return symbol_candidate;
    }
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

    auto* impl = reinterpret_cast<const ProtoStringImplementation*>(
        toImpl<const ProtoStringImplementation>(strObj));
    if (!impl) return nullptr;
    uint64_t hash = impl->implGetHash();
    int shard_idx = shardIndex(hash);
    const Shard& shard = shards[shard_idx];

    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(shard.mutex));
    for (const Bucket* b = shard.head; b; b = b->next) {
        if (b->content_hash == hash &&
            contentEqual(ctx, b->symbol, strObj))
            return b->symbol;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// removeWeak — called by GC sweep to evict a collected weak symbol.
// ---------------------------------------------------------------------------
void SymbolTable::removeWeak(uint64_t content_hash, const ProtoObject* symbol) {
    int shard_idx = shardIndex(content_hash);
    Shard& shard  = shards[shard_idx];
    std::lock_guard<std::mutex> lock(shard.mutex);
    Bucket** prev = &shard.head;
    for (Bucket* b = shard.head; b; b = b->next) {
        if (b->content_hash == content_hash && b->symbol == symbol && !b->is_strong) {
            *prev = b->next;
            delete b;
            return;
        }
        prev = &b->next;
    }
}

} // namespace proto
