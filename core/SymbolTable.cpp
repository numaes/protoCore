/*
 * SymbolTable.cpp — the process-wide string interning table: a lock-free,
 * write-once hash trie.
 *
 * P3 (2026-09-24): ONE table per PROCESS, reached through globalSymbolTable().
 * ProtoSpace::symbolTable is a BORROWED pointer to it and ~ProtoSpace must NOT
 * free it.
 *
 * Every interned string (symbol) is PERENNIAL. `intern` builds the canonical
 * rope with a NULL ProtoContext, which routes every Cell allocation through
 * `posix_memalign` directly: the Cells are never enrolled in any thread
 * freelist or context young-generation chain, so the GC's mark/sweep and root
 * collection never see them — they live for the lifetime of the process.
 * There is no "weak"/collectible symbol variant: a name, once interned, must
 * keep its canonical pointer forever for pointer-identity symbol comparison
 * to be sound. See DESIGN.md § "Perpetual allocations via NULL ProtoContext".
 *
 * The index follows from the same fact: nothing is ever removed, so it needs
 * no lock and no reclamation scheme.
 *
 *   Hash.   The 64-bit FNV-1a content hash (the same over UTF-8 bytes and over
 *           a rope's leaves, computeContentHash), then fmix64, so every bit of
 *           the index depends on every byte.
 *
 *   Shape.  Level 0 is an array of 1024 atomic slots (10 hash bits).  Each
 *           deeper level is a node of 64 atomic slots (6 bits); levels 1..9
 *           consume the remaining 54 bits.  A slot holds nothing, a leaf
 *           {hash, symbol}, a child node, or -- for a full 64-bit collision
 *           only -- a chain of leaves sharing one hash.  Nodes, leaves and
 *           chain entries are C++ allocations (never Cells) and are never
 *           freed while the table lives.
 *
 *   Write once.  A published node, leaf or chain entry is immutable.  A slot
 *           changes from empty to a leaf, or from a leaf (or chain) to a child
 *           node, once; a chain slot also changes by prepending.  Every change
 *           is a compare-and-swap with release order; every read an acquire
 *           load.  Nothing a reader has seen ever changes.
 *
 *   Lookup. One acquire load per level; an empty slot means absent; a leaf is
 *           compared by hash, then by content in place (no copy, no
 *           allocation); a node is descended.  No lock, no allocation, no
 *           store.
 *
 *   Insert. The perennial candidate symbol is built before the trie is
 *           touched.  The insert descends; an empty slot is claimed with a
 *           CAS.  A slot holding a leaf of a different hash is replaced by a
 *           child node that already holds that leaf at its next-level index;
 *           the insert then continues one level down.  A lost CAS re-reads
 *           the slot and continues: whatever won is either the same spelling
 *           (adopted) or a node to descend.  Index memory built for a CAS that
 *           lost was never published, so it is freed at once.
 *
 * No mutex is taken anywhere in intern or lookup, so the table is ordered
 * against no lock: interning from a critical section, a collector or another
 * space's thread is always safe.
 */

#include "proto_internal.h"
#include <cstring>
#include <new>
#include <string>

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

proto_ulong globalSymbolCount() { return globalSymbolTable().entryCount(); }

namespace {

// Slot tags in the low bits of a slot word.  Leaves, nodes and chain entries
// are allocated with at least 8-byte alignment.
constexpr uintptr_t kTagMask  = 0x3;
constexpr uintptr_t kTagLeaf  = 0x0;
constexpr uintptr_t kTagNode  = 0x1;
constexpr uintptr_t kTagChain = 0x2;

using Leaf  = SymbolTable::Leaf;
using Chain = SymbolTable::Chain;
using Node  = SymbolTable::Node;

inline uintptr_t tagOf(uintptr_t w) { return w & kTagMask; }
inline const Leaf*  asLeaf(uintptr_t w)  { return reinterpret_cast<const Leaf*>(w); }
inline Node*        asNode(uintptr_t w)  { return reinterpret_cast<Node*>(w & ~kTagMask); }
inline const Chain* asChain(uintptr_t w) { return reinterpret_cast<const Chain*>(w & ~kTagMask); }
inline uintptr_t wordOf(const Leaf* l)   { return reinterpret_cast<uintptr_t>(l) | kTagLeaf; }
inline uintptr_t wordOf(const Node* n)   { return reinterpret_cast<uintptr_t>(n) | kTagNode; }
inline uintptr_t wordOf(const Chain* c)  { return reinterpret_cast<uintptr_t>(c) | kTagChain; }

Node* newNode() {
    Node* n = new Node;
    for (auto& s : n->slots) s.store(0, std::memory_order_relaxed);
    return n;
}

// The slots of `level`: the table's root array at level 0, a node's below.
inline std::atomic<uintptr_t>& slotAt(std::atomic<uintptr_t>* slots, uint64_t hash, int level) {
    return slots[SymbolTable::indexAt(hash, level)];
}

// MurmurHash3's 64-bit finalizer.
inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

// ---------------------------------------------------------------------------
// contentHashOfUTF8 — the content hash of a run of UTF-8 bytes.
//
// Must stay identical to computeContentHash (core/ProtoString.cpp), which is
// FNV-1a over the leaf bytes in order and is the hash of every heap string: a
// lookup keyed by bytes has to reach the same leaf as one keyed by a
// ProtoString.
// ---------------------------------------------------------------------------
uint64_t contentHashOfUTF8(const uint8_t* bytes, size_t len) {
    uint64_t h = 14695981039346656037ULL;   // FNV offset basis
    for (size_t i = 0; i < len; ++i)
        h = (h ^ bytes[i]) * 1099511628211ULL;
    return h;
}

// The UTF-8 bytes of a rope, leaf by leaf, left to right, read in place.
class RopeChunks {
public:
    explicit RopeChunks(const ProtoObject* root) { if (root) stack_[top_++] = root; }

    // The next non-empty run of bytes; false at the end of the rope.
    bool next(const uint8_t*& bytes, size_t& len) {
        while (top_ > 0) {
            const ProtoObject* cur = stack_[--top_];
            if (!cur) continue;
            if (StringLeafNode::isStringLeafNode(cur)) {
                const auto* leaf = StringLeafNode::fromObject(cur);
                if (leaf->byte_count == 0) continue;
                bytes = leaf->utf8_payload;
                len = leaf->byte_count;
                return true;
            }
            if (StringInternalNode::isStringInternalNode(cur)) {
                const auto* n = StringInternalNode::fromObject(cur);
                if (top_ + 2 > kMaxDepth) std::abort();   // ropes are balanced
                stack_[top_++] = n->right;
                stack_[top_++] = n->left;
            }
        }
        return false;
    }

private:
    static constexpr int kMaxDepth = 64;
    const ProtoObject* stack_[kMaxDepth];
    int top_ = 0;
};

// Does the rope at `root` spell exactly `bytes[0, len)`?
bool ropeEqualsBytes(const ProtoObject* root, const uint8_t* bytes, size_t len) {
    RopeChunks chunks(root);
    const uint8_t* p;
    size_t n;
    size_t at = 0;
    while (chunks.next(p, n)) {
        if (n > len - at || std::memcmp(p, bytes + at, n) != 0) return false;
        at += n;
    }
    return at == len;
}

// Do two ropes spell the same bytes?  Their leaf boundaries may differ.
bool ropesEqual(const ProtoObject* a, const ProtoObject* b) {
    RopeChunks ca(a), cb(b);
    const uint8_t* pa = nullptr;
    const uint8_t* pb = nullptr;
    size_t na = 0, nb = 0;
    for (;;) {
        // A run left with no bytes is refilled; a rope that has none left
        // keeps a zero count.
        if (na == 0 && !ca.next(pa, na)) na = 0;
        if (nb == 0 && !cb.next(pb, nb)) nb = 0;
        if (na == 0 || nb == 0) return na == 0 && nb == 0;
        const size_t n = na < nb ? na : nb;
        if (std::memcmp(pa, pb, n) != 0) return false;
        pa += n; na -= n;
        pb += n; nb -= n;
    }
}

// ---------------------------------------------------------------------------
// Losing candidates.
//
// A candidate symbol that lost the race to another thread's identical
// spelling is perennial like every symbol string (its cells come from
// posix_memalign and no cycle reclaims them).  Retained memory must stay
// reachable, or LeakSanitizer reports it at exit
// (GlobalInterning.ConcurrentInterningAcrossSpacesAgreesOnOnePointer did), so
// each one is pushed on a never-destroyed, lock-free, prepend-only stack.
// ---------------------------------------------------------------------------
struct LostCandidate {
    const ProtoObject*   symbol;
    const LostCandidate* next;
};
std::atomic<const LostCandidate*>& lostCandidates() {
    static auto* head = new std::atomic<const LostCandidate*>(nullptr);
    return *head;
}
void keepLostCandidate(const ProtoObject* symbol) {
    auto* entry = new LostCandidate{symbol, nullptr};
    auto& head = lostCandidates();
    const LostCandidate* h = head.load(std::memory_order_relaxed);
    do {
        entry->next = h;
    } while (!head.compare_exchange_weak(h, entry, std::memory_order_release,
                                         std::memory_order_relaxed));
}

}  // namespace

// What a lookup compares a published symbol against: a run of UTF-8 bytes or
// the root of a heap string's rope.
struct SymbolTable::Key {
    const uint8_t*     bytes = nullptr;
    size_t             len = 0;
    const ProtoObject* rope = nullptr;

    bool matches(const ProtoObject* symbol) const {
        const ProtoObject* root = heapStringRoot(symbol);
        return rope ? ropesEqual(root, rope) : ropeEqualsBytes(root, bytes, len);
    }
};

SymbolTable::SymbolTable(HashMix mix) : mix_(mix) {
    for (auto& s : root_) s.store(0, std::memory_order_relaxed);
}

// The process-wide table is never destroyed; private tables (tests) are.  The
// index is freed, never the symbols: they are perennial.
SymbolTable::~SymbolTable() {
    struct Freer {
        static void slot(uintptr_t w) {
            if (!w) return;
            switch (tagOf(w)) {
                case kTagLeaf:
                    delete asLeaf(w);
                    break;
                case kTagNode: {
                    Node* n = asNode(w);
                    for (auto& s : n->slots) slot(s.load(std::memory_order_relaxed));
                    delete n;
                    break;
                }
                case kTagChain:
                    for (const Chain* c = asChain(w); c;) {
                        const Chain* next = c->next;
                        delete c->leaf;
                        delete c;
                        c = next;
                    }
                    break;
            }
        }
    };
    for (auto& s : root_) Freer::slot(s.load(std::memory_order_relaxed));
}

proto_ulong SymbolTable::entryCount() const {
    return count_.load(std::memory_order_relaxed);
}

bool SymbolTable::isSymbol(const ProtoObject* obj) {
    if (!obj) return false;
    ProtoObjectPointer pa{};
    pa.oid = obj;
    return pa.op.pointer_tag == POINTER_TAG_SYMBOL;
}

uint64_t SymbolTable::mixed(uint64_t contentHash) const {
    return mix_ ? mix_(contentHash) : fmix64(contentHash);
}

// ---------------------------------------------------------------------------
// find — the lock-free lookup.
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::find(uint64_t hash, const Key& key) const {
    const std::atomic<uintptr_t>* slots = root_;
    for (int level = 0; level <= kMaxLevel; ++level) {
        const uintptr_t w = slots[indexAt(hash, level)].load(std::memory_order_acquire);
        if (!w) return nullptr;
        switch (tagOf(w)) {
            case kTagLeaf: {
                const Leaf* leaf = asLeaf(w);
                return leaf->hash == hash && key.matches(leaf->symbol) ? leaf->symbol : nullptr;
            }
            case kTagChain: {
                const Chain* c = asChain(w);
                if (c->leaf->hash != hash) return nullptr;
                for (; c; c = c->next)
                    if (key.matches(c->leaf->symbol)) return c->leaf->symbol;
                return nullptr;
            }
            default:
                slots = asNode(w)->slots;
                break;
        }
    }
    return nullptr;   // unreachable: level kMaxLevel holds no node
}

// ---------------------------------------------------------------------------
// insert — publish `candidate` or adopt the symbol that won.
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::insert(uint64_t hash, const Key& /*callerKey*/,
                                       const ProtoObject* candidate) {
    // Compare against the candidate itself: it is perennial, while the
    // caller's string is an ordinary heap string.
    Key key;
    key.rope = heapStringRoot(candidate);
    Leaf* mine = new Leaf{hash, candidate};
    std::atomic<uintptr_t>* slots = root_;
    int level = 0;
    for (;;) {
        std::atomic<uintptr_t>& slot = slots[indexAt(hash, level)];
        uintptr_t w = slot.load(std::memory_order_acquire);

        if (!w) {
            if (slot.compare_exchange_strong(w, wordOf(mine), std::memory_order_release,
                                             std::memory_order_acquire)) {
                count_.fetch_add(1, std::memory_order_relaxed);
                return candidate;
            }
            continue;   // `w` now holds what won: examine it
        }

        if (tagOf(w) == kTagNode) {
            slots = asNode(w)->slots;
            ++level;
            continue;
        }

        // A leaf or a chain: every leaf it holds has the same hash.
        const Leaf* first = tagOf(w) == kTagLeaf ? asLeaf(w) : asChain(w)->leaf;
        if (first->hash == hash) {
            // The same full hash: the spelling is here already, or this is a
            // full 64-bit collision and the slot is (or becomes) a chain.
            if (tagOf(w) == kTagLeaf) {
                if (key.matches(first->symbol)) {
                    delete mine;
                    keepLostCandidate(candidate);
                    return first->symbol;
                }
            } else {
                for (const Chain* c = asChain(w); c; c = c->next) {
                    if (key.matches(c->leaf->symbol)) {
                        delete mine;
                        keepLostCandidate(candidate);
                        return c->leaf->symbol;
                    }
                }
            }
            const bool fromLeaf = tagOf(w) == kTagLeaf;
            const Chain* rest = fromLeaf ? new Chain{first, nullptr} : asChain(w);
            auto* head = new Chain{mine, rest};
            if (slot.compare_exchange_strong(w, wordOf(head), std::memory_order_release,
                                             std::memory_order_acquire)) {
                count_.fetch_add(1, std::memory_order_relaxed);
                return candidate;
            }
            // Lost: the entries built here were never published.
            delete head;
            if (fromLeaf) delete rest;
            continue;
        }

        // A different hash shares this slot: push what is here one level down
        // into a new node, then continue into that node.
        if (level == kMaxLevel) std::abort();   // 64 bits consumed with different hashes: impossible
        Node* child = newNode();
        child->slots[indexAt(first->hash, level + 1)].store(w, std::memory_order_relaxed);
        if (slot.compare_exchange_strong(w, wordOf(child), std::memory_order_release,
                                         std::memory_order_acquire)) {
            slots = child->slots;
            ++level;
            continue;
        }
        delete child;   // never published
    }
}

// ---------------------------------------------------------------------------
// The public entry points.
// ---------------------------------------------------------------------------

const ProtoObject* SymbolTable::lookupUTF8(ProtoContext* /*ctx*/,
                                            const uint8_t* bytes, size_t len) const {
    Key key;
    key.bytes = bytes;
    key.len = len;
    return find(mixed(contentHashOfUTF8(bytes, len)), key);
}

const ProtoObject* SymbolTable::lookupByContent(ProtoContext* ctx,
                                                  const ProtoObject* strObj) const {
    if (!strObj) return nullptr;
    ProtoObjectPointer pa{};
    pa.oid = strObj;
    // POINTER_TAG_SYMBOL — the pointer IS the canonical identity.
    if (pa.op.pointer_tag == POINTER_TAG_SYMBOL) return strObj;
    // Embedded values: canonical by construction, never in the table.
    if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) return strObj;
    // A spelling of at most INLINE_STRING_MAX_BYTES bytes: its inline form.
    if (const ProtoObject* inl = inlineFormOfString(ctx, strObj)) return inl;
    const ProtoObject* root = heapStringRoot(strObj);
    if (!root) return nullptr;
    Key key;
    key.rope = root;
    return find(mixed(computeContentHash(root)), key);
}

// ---------------------------------------------------------------------------
// intern — the canonical symbol for a string, interning it on first sight.
//
// The candidate is built before the trie is touched: building it allocates
// Cells, and the insert itself allocates nothing but index memory.  Every
// symbol's Cells are perpetual (NULL ProtoContext): a symbol's identity must
// outlive every context and thread that references it.
// ---------------------------------------------------------------------------
const ProtoObject* SymbolTable::intern(ProtoContext* ctx,
                                        const ProtoObject* strObj) {
    if (!strObj) return strObj;
    ProtoObjectPointer pa{};
    pa.oid = strObj;
    if (pa.op.pointer_tag == POINTER_TAG_SYMBOL) return strObj;
    if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) return strObj;
    if (const ProtoObject* inl = inlineFormOfString(ctx, strObj)) return inl;
    const ProtoObject* root = heapStringRoot(strObj);
    if (!root) return strObj;   // not a string: nothing to intern
    Key key;
    key.rope = root;
    const uint64_t hash = mixed(computeContentHash(root));
    // Already interned?  The copy below is perennial: for a spelling the
    // table holds it would be pure leak.
    if (const ProtoObject* existing = find(hash, key)) return existing;
    std::string utf8;
    reinterpret_cast<const ProtoString*>(strObj)->toUTF8String(ctx, utf8);
    const ProtoObject* copy = buildRopeFromUTF8(
        /*ctx=*/nullptr, reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size());
    return insert(hash, key, heapStringFromRoot(copy, /*symbol=*/true));
}

const ProtoObject* SymbolTable::internUTF8(ProtoContext* /*ctx*/,
                                            const uint8_t* bytes, size_t len) {
    Key key;
    key.bytes = bytes;
    key.len = len;
    const uint64_t hash = mixed(contentHashOfUTF8(bytes, len));
    if (const ProtoObject* existing = find(hash, key)) return existing;
    const ProtoObject* root = buildRopeFromUTF8(/*ctx=*/nullptr, bytes, len);
    return insert(hash, key, heapStringFromRoot(root, /*symbol=*/true));
}

// ---------------------------------------------------------------------------
// shape — a full walk, for diagnostics and tests.
// ---------------------------------------------------------------------------
SymbolTable::Shape SymbolTable::shape() const {
    Shape s;
    s.indexBytes = sizeof(root_);
    struct Walker {
        Shape& s;
        void slot(uintptr_t w, proto_ulong level) {
            if (!w) return;
            switch (tagOf(w)) {
                case kTagLeaf:
                    ++s.leaves;
                    s.indexBytes += sizeof(Leaf);
                    if (level > s.maxDepth) s.maxDepth = level;
                    break;
                case kTagNode: {
                    ++s.nodes;
                    s.indexBytes += sizeof(Node);
                    for (const auto& c : asNode(w)->slots)
                        slot(c.load(std::memory_order_acquire), level + 1);
                    break;
                }
                case kTagChain:
                    for (const Chain* c = asChain(w); c; c = c->next) {
                        ++s.leaves;
                        ++s.chainEntries;
                        s.indexBytes += sizeof(Leaf) + sizeof(Chain);
                    }
                    if (level > s.maxDepth) s.maxDepth = level;
                    break;
            }
        }
    } walker{s};
    for (const auto& r : root_) walker.slot(r.load(std::memory_order_acquire), 0);
    return s;
}

} // namespace proto
