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
// ---------------------------------------------------------------------------
const ProtoStringImplementation* SymbolTable::normalizeForSymbol(
        ProtoContext* ctx, const ProtoObject* strObj) {
    std::string utf8;
    reinterpret_cast<const ProtoString*>(strObj)->toUTF8String(ctx, utf8);
    return ProtoStringImplementation::fromUTF8Bytes(
        ctx,
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
    const ProtoStringImplementation* normalized = normalizeForSymbol(ctx, strObj);
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
