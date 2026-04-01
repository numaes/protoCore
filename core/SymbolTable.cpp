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
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::intern(ProtoContext* ctx,
                                        const ProtoObject* strObj,
                                        bool is_strong) {
    if (!strObj) return strObj;

    // Already a symbol — return as-is.
    {
        ProtoObjectPointer pa{};
        pa.oid = strObj;
        if (pa.op.pointer_tag == POINTER_TAG_SYMBOL) return strObj;
        // Embedded values (inline strings, small integers, etc.) cannot be
        // interned as symbols — return unchanged.
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) return strObj;
    }

    // Compute hash for shard selection.
    ProtoObjectPointer pa{};
    pa.oid = strObj;
    uint64_t hash = 0;
    if (pa.op.pointer_tag == POINTER_TAG_STRING) {
        const ProtoStringImplementation* impl =
            reinterpret_cast<const ProtoStringImplementation*>(pa.stringImplementation);
        hash = impl->implGetHash();
    } else {
        // Inline or other string form — derive hash from UTF-8 content.
        std::string utf8;
        reinterpret_cast<const ProtoString*>(strObj)->toUTF8String(ctx, utf8);
        // FNV-1a over the UTF-8 bytes to produce a stable hash.
        hash = 14695981039346656037ULL;
        for (unsigned char c : utf8) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ULL;
        }
    }

    int shard_idx = shardIndex(hash);
    Shard& shard = shards[shard_idx];

    std::lock_guard<std::mutex> lock(shard.mutex);

    // Search existing buckets for a content-equal symbol.
    for (Bucket* b = shard.head; b; b = b->next) {
        if (b->content_hash == hash &&
            contentEqual(ctx, b->symbol, strObj)) {
            return b->symbol;
        }
    }

    // Not found — create a new symbol and insert into the shard.
    const ProtoStringImplementation* normalized = normalizeForSymbol(ctx, strObj);
    const ProtoObject* symbol = reinterpret_cast<const ProtoObject*>(
        normalized->implAsSymbol(ctx));

    Bucket* bucket = new Bucket{ hash, symbol, is_strong, shard.head };
    shard.head = bucket;
    return symbol;
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
        if (b->symbol == symbol && !b->is_strong) {
            *prev = b->next;
            delete b;
            return;
        }
        prev = &b->next;
    }
}

} // namespace proto
