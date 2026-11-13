/*
 * ProtoList.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <algorithm> // For std::max
#include <climits>

namespace proto {

    namespace { // Anonymous namespace for file-local helpers

        /**
         * @brief Safely gets the size of a node.
         * Checks for null and 64-byte alignment before dereferencing.
         * A tagged handle passed by mistake will be unaligned and treated as size 0.
         */
        inline proto_ulong get_node_size(const ProtoListImplementation* node) {
            if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) {
                return 0;
            }
            return node->size;
        }

        /**
         * @brief Safely gets the height of a node.
         * Checks for null and 64-byte alignment before dereferencing.
         * A tagged handle passed by mistake will be unaligned and treated as height 0.
         */
        inline int get_node_height(const ProtoListImplementation* node) {
            if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) {
                return 0;
            }
            return node->height;
        }

        int getBalance(const ProtoListImplementation* node) {
            if (!node || node->isEmpty) return 0;
            return get_node_height(node->previousNode) - get_node_height(node->nextNode);
        }

        const ProtoListImplementation* rightRotate(ProtoContext* context, const ProtoListImplementation* y) {
            const ProtoListImplementation* x = y->previousNode;
            const ProtoListImplementation* T2 = x ? x->nextNode : nullptr;
            auto* new_y = new (context) ProtoListImplementation(context, y->value, false, T2, y->nextNode);
            return new (context) ProtoListImplementation(context, x->value, false, x->previousNode, new_y);
        }

        const ProtoListImplementation* leftRotate(ProtoContext* context, const ProtoListImplementation* x) {
            const ProtoListImplementation* y = x->nextNode;
            const ProtoListImplementation* T2 = y ? y->previousNode : nullptr;
            auto* new_x = new (context) ProtoListImplementation(context, x->value, false, x->previousNode, T2);
            return new (context) ProtoListImplementation(context, y->value, false, new_x, y->nextNode);
        }

        const ProtoListImplementation* rebalance(ProtoContext* context, const ProtoListImplementation* node) {
            int balance = getBalance(node);

            // Left Left Case
            if (balance > 1 && getBalance(node->previousNode) >= 0)
                return rightRotate(context, node);

            // Left Right Case
            if (balance > 1 && getBalance(node->previousNode) < 0) {
                const ProtoListImplementation* new_prev = leftRotate(context, node->previousNode);
                return rightRotate(context, new (context) ProtoListImplementation(context, node->value, false, new_prev, node->nextNode));
            }

            // Right Right Case
            if (balance < -1 && getBalance(node->nextNode) <= 0)
                return leftRotate(context, node);

            // Right Left Case
            if (balance < -1 && getBalance(node->nextNode) > 0) {
                const ProtoListImplementation* new_next = rightRotate(context, node->nextNode);
                return leftRotate(context, new (context) ProtoListImplementation(context, node->value, false, node->previousNode, new_next));
            }

            return node;
        }

    } // end anonymous namespace

    //=========================================================================
    // ProtoListImplementation
    //=========================================================================

    ProtoListImplementation::ProtoListImplementation(
        ProtoContext* context,
        const ProtoObject* v,
        bool empty,
        const ProtoListImplementation* prev,
        const ProtoListImplementation* next
    ) : Cell(context), value(empty ? nullptr : v), previousNode(empty ? nullptr : prev), nextNode(empty ? nullptr : next),
        hash(0),
        size(empty ? 0 : get_node_size(prev) + get_node_size(next) + 1),
        height(empty ? 0 : 1 + std::max(get_node_height(prev), get_node_height(next))),
        isEmpty(empty)
    {
    }

    const ProtoObject* ProtoListImplementation::implGetAt(ProtoContext* context, int index) const {
        if (isEmpty) return nullptr;
        const proto_ulong left_size = previousNode ? previousNode->size : 0;
        if (index < left_size) {
            return previousNode->implGetAt(context, index);
        }
        if (index == left_size) {
            return value;
        }
        return nextNode->implGetAt(context, toIndex(index - left_size - 1));
    }

    namespace {
        // Element equality for has(): identity, integers by value (SmallInteger
        // and LargeInteger of any size, through Integer::compare — asLong
        // throws beyond long long), strings by content.  Doubles and other
        // objects match by identity only.
        bool listElementEquals(ProtoContext* context, const ProtoObject* element,
                               const ProtoObject* target) {
            if (element == target) return true;
            if (!element) return false;
            if (element->isInteger(context) && target->isInteger(context)) {
                return Integer::compare(context, element, target) == 0;
            }
            if (element->isString(context) && target->isString(context)) {
                return element->asString(context)->cmp_to_string(context, target->asString(context)) == 0;
            }
            return false;
        }
    }

    bool ProtoListImplementation::implHas(proto::ProtoContext* context, const proto::ProtoObject* targetValue) const {
        if (!targetValue) return false;
        if (isEmpty) return false;
        
        if (listElementEquals(context, value, targetValue)) return true;
        if (previousNode && previousNode->implHas(context, targetValue)) return true;
        if (nextNode && nextNode->implHas(context, targetValue)) return true;
        return false;
    }

    const ProtoListImplementation* ProtoListImplementation::implSetAt(ProtoContext* context, int index, const ProtoObject* newValue) const {
        if (isEmpty || index < 0 || index >= size) {
            return this;
        }

        const proto_ulong left_size = previousNode ? previousNode->size : 0;

        if (index < left_size) {
            const ProtoListImplementation* new_prev = previousNode->implSetAt(context, index, newValue);
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, new_prev, nextNode));
        }
        if (index == left_size) {
            return rebalance(context, new (context) ProtoListImplementation(context, newValue, false, previousNode, nextNode));
        }

        const ProtoListImplementation* new_next = nextNode->implSetAt(context, toIndex(index - left_size - 1), newValue);
        return rebalance(context, new (context) ProtoListImplementation(context, value, false, previousNode, new_next));
    }

    const ProtoListImplementation* ProtoListImplementation::implInsertAt(ProtoContext* context, int index, const ProtoObject* newValue) const {
        if (isEmpty) {
            return new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
        }

        const proto_ulong left_size = previousNode ? previousNode->size : 0;

        if (index <= left_size) {
            const ProtoListImplementation* new_prev = previousNode
                ? previousNode->implInsertAt(context, index, newValue)
                : new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, new_prev, nextNode));
        } else {
            const ProtoListImplementation* new_next = nextNode
                ? nextNode->implInsertAt(context, toIndex(index - left_size - 1), newValue)
                : new (context) ProtoListImplementation(context, newValue, false, nullptr, nullptr);
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, previousNode, new_next));
        }
    }


    const ProtoListImplementation* ProtoListImplementation::implAppendLast(ProtoContext* context, const ProtoObject* newValue) const {
        return implInsertAt(context, toIndex(size), newValue);
    }

    const ProtoListImplementation* ProtoListImplementation::implRemoveAt(ProtoContext* context, int index) const {
        if (isEmpty || index < 0 || index >= size) {
            return this;
        }

        const proto_ulong left_size = previousNode ? previousNode->size : 0;

        if (index < left_size) {
            const ProtoListImplementation* new_prev = previousNode->implRemoveAt(context, index);
            if (new_prev && new_prev->size == 0) new_prev = nullptr;
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, new_prev, nextNode));
        }
        else if (index == left_size) {
            // Remove current value
            if (!previousNode && !nextNode) {
                // Became empty
                return new (context) ProtoListImplementation(context, nullptr, true, nullptr, nullptr);
            }
            if (!previousNode) return rebalance(context, nextNode);
            if (!nextNode) return rebalance(context, previousNode);

            // Both exist. Promote from left.
            const ProtoObject* new_val = previousNode->implGetAt(context, toIndex(previousNode->size - 1));
            const ProtoListImplementation* new_prev = previousNode->implRemoveAt(context, toIndex(previousNode->size - 1));
            if (new_prev && new_prev->size == 0) new_prev = nullptr;
            return rebalance(context, new (context) ProtoListImplementation(context, new_val, false, new_prev, nextNode));
        }
        else {
            // index > left_size
            const ProtoListImplementation* new_next = nextNode->implRemoveAt(context, toIndex(index - left_size - 1));
            if (new_next && new_next->size == 0) new_next = nullptr;
            return rebalance(context, new (context) ProtoListImplementation(context, value, false, previousNode, new_next));
        }
    }

    void ProtoListImplementation::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (const Cell* c = ProtoObject::asCellPointer(value)) method(context, self, c);
        if (const Cell* c = ProtoObject::asCellPointer(reinterpret_cast<const ProtoObject*>(previousNode))) {
            method(context, self, c);
        }
        if (const Cell* c = ProtoObject::asCellPointer(reinterpret_cast<const ProtoObject*>(nextNode))) {
            method(context, self, c);
        }
    }

    const ProtoObject* ProtoListImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.oid;
    }

    const ProtoList* ProtoListImplementation::asProtoList(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.list;
    }

    const ProtoListIteratorImplementation* ProtoListImplementation::implGetIterator(ProtoContext* context) const {
        // Build the tagged base directly; the iterator dispatches on the tag.
        ProtoObjectPointer p;
        p.listImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST;
        return new (context) ProtoListIteratorImplementation(context, p.oid, 0);
    }

    //=========================================================================
    // ProtoListSmallImplementation
    //=========================================================================

    ProtoListSmallImplementation::ProtoListSmallImplementation(
        ProtoContext* context, unsigned n, const ProtoObject* const* items
    ) : Cell(context), size(n)
    {
        for (unsigned i = 0; i < MAX_INLINE; ++i) {
            slots[i] = (i < n) ? items[i] : nullptr;
        }
    }

    const ProtoObject* ProtoListSmallImplementation::implGetAt(ProtoContext* context, int index) const {
        if (index < 0 || static_cast<proto_ulong>(index) >= size) return nullptr;
        return slots[index];
    }

    bool ProtoListSmallImplementation::implHas(ProtoContext* context, const ProtoObject* targetValue) const {
        if (!targetValue) return false;
        for (proto_ulong i = 0; i < size; ++i) {
            if (listElementEquals(context, slots[i], targetValue)) return true;
        }
        return false;
    }

    const ProtoObject* ProtoListSmallImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listSmallImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_SMALL;
        return p.oid;
    }

    const ProtoList* ProtoListSmallImplementation::asProtoList(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listSmallImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_SMALL;
        return p.list;
    }

    void ProtoListSmallImplementation::processReferences(
        ProtoContext* context, void* self,
        void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        for (proto_ulong i = 0; i < size; ++i) {
            if (const Cell* c = ProtoObject::asCellPointer(slots[i])) {
                method(context, self, c);
            }
        }
    }

    //=========================================================================
    // ProtoListTrieNode / ProtoListTrieImplementation (the trie form)
    //=========================================================================
    //
    // A persistent vector with six slots per node, so that every node is one
    // 64-byte cell.  Element i (i < tailOffset) lives in the leaf reached by
    // the base-6 digits of i, most significant first; the last 1..6 elements
    // live in `tail`.  Every leaf under `tree` is full, so tailOffset is a
    // multiple of 6 and element i always sits in slot i % 6 of its leaf, in
    // the tree or in the tail.
    //
    // Costs: getAt O(levels) reads and no cell; appendLast 2 cells (tail and
    // root), plus the path to the new leaf every sixth append; setAt and
    // removeLast copy one path.  levels = ceil(log6(size)) - 1, 6 for 100 k
    // elements.
    //
    // GC: nodes are immutable once reachable; every public entry point that
    // builds nodes holds a ProtoContext::CriticalSection, as the AVL mutators
    // do, so a half-built path held in C++ locals is never swept.

    ProtoListTrieNode::ProtoListTrieNode(ProtoContext* context, unsigned n, const void* const* items)
        : Cell(context)
    {
        for (unsigned i = 0; i < WIDTH; ++i) slots[i] = (i < n) ? items[i] : nullptr;
    }

    const ProtoObject* ProtoListTrieNode::implAsObject(ProtoContext*) const {
        // Internal cell: never handed to the object API (same rule as the
        // MPSC queue's node cells), so the raw, untagged address.
        return reinterpret_cast<const ProtoObject*>(this);
    }

    void ProtoListTrieNode::processReferences(
        ProtoContext* context, void* self,
        void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        // An inner node's slots are raw node addresses (tag bits zero), a
        // leaf's are elements as stored; asCellPointer reports the cells of
        // both and skips nullptr and embedded values.
        for (unsigned i = 0; i < WIDTH; ++i) {
            if (const Cell* c = ProtoObject::asCellPointer(static_cast<const ProtoObject*>(slots[i]))) {
                method(context, self, c);
            }
        }
    }

    ProtoListTrieImplementation::ProtoListTrieImplementation(
        ProtoContext* context, proto_ulong n, const ProtoListTrieNode* t,
        const ProtoListTrieNode* tl, unsigned lv, unsigned tlen)
        : Cell(context), size(n), tree(t), tail(tl),
          levels(static_cast<unsigned char>(lv)), tailLen(static_cast<unsigned char>(tlen))
    {
    }

    namespace {
        using TrieNode = ProtoListTrieNode;
        using Trie = ProtoListTrieImplementation;
        constexpr unsigned TW = ProtoListTrieNode::WIDTH;
        // 6^25 > 2^64: no list can need more digits than this.
        constexpr unsigned kMaxTrieDigits = 26;

        // The base-6 digits of `index`, least significant first, for digits
        // 0..levels.  The divisor is a constant, so the compiler multiplies.
        inline void trieDigits(proto_ulong index, unsigned levels, unsigned* d) {
            for (unsigned k = 0; k <= levels; ++k) {
                d[k] = static_cast<unsigned>(index % TW);
                index /= TW;
            }
        }

        // 6^(levels + 1): how many elements a tree of `levels` inner levels holds.
        inline proto_ulong trieCapacity(unsigned levels) {
            proto_ulong c = TW;
            for (unsigned k = 0; k < levels; ++k) c *= TW;
            return c;
        }

        inline const TrieNode* childAt(const TrieNode* n, unsigned slot) {
            return static_cast<const TrieNode*>(n->slots[slot]);
        }

        // `leaf` under `level` single-child inner nodes.
        const TrieNode* newPath(ProtoContext* context, unsigned level, const TrieNode* leaf) {
            const TrieNode* node = leaf;
            for (unsigned k = 0; k < level; ++k) {
                const void* s[1] = {node};
                node = new (context) TrieNode(context, 1, s);
            }
            return node;
        }

        // A copy of `node` (an inner node at `level` >= 1) with the full leaf
        // `leaf`, whose first element has the digits `d`, hung in its place.
        const TrieNode* pushTail(ProtoContext* context, unsigned level, const TrieNode* node,
                                 const unsigned* d, const TrieNode* leaf) {
            const void* s[TW];
            for (unsigned i = 0; i < TW; ++i) s[i] = node->slots[i];
            const unsigned slot = d[level];
            if (level == 1) {
                s[slot] = leaf;
            } else {
                const TrieNode* child = childAt(node, slot);
                s[slot] = child ? pushTail(context, level - 1, child, d, leaf)
                                : newPath(context, level - 1, leaf);
            }
            return new (context) TrieNode(context, TW, s);
        }

        // A copy of the path to the element with digits `d`, the element replaced.
        const TrieNode* setInTree(ProtoContext* context, unsigned level, const TrieNode* node,
                                  const unsigned* d, const ProtoObject* value) {
            const void* s[TW];
            for (unsigned i = 0; i < TW; ++i) s[i] = node->slots[i];
            const unsigned slot = d[level];
            if (level == 0) s[slot] = value;
            else s[slot] = setInTree(context, level - 1, childAt(node, slot), d, value);
            return new (context) TrieNode(context, TW, s);
        }

        // `node` (an inner node at `level` >= 1) without its last leaf, whose
        // first element has the digits `d`; nullptr when nothing is left.
        const TrieNode* popTail(ProtoContext* context, unsigned level, const TrieNode* node,
                                const unsigned* d) {
            const unsigned slot = d[level];
            const TrieNode* rest = nullptr;
            if (level > 1) {
                rest = popTail(context, level - 1, childAt(node, slot), d);
            }
            if (!rest && slot == 0) return nullptr;
            const void* s[TW];
            for (unsigned i = 0; i < TW; ++i) s[i] = node->slots[i];
            s[slot] = rest;
            return new (context) TrieNode(context, TW, s);
        }

        // Calls fn(element) for every element of the full leaves under `node`.
        template <typename Fn>
        bool walkLeaves(unsigned level, const TrieNode* node, Fn& fn) {
            if (level == 0) {
                for (unsigned i = 0; i < TW; ++i) {
                    if (fn(static_cast<const ProtoObject*>(node->slots[i]))) return true;
                }
                return false;
            }
            for (unsigned i = 0; i < TW && node->slots[i]; ++i) {
                if (walkLeaves(level - 1, childAt(node, i), fn)) return true;
            }
            return false;
        }

        // Calls fn(element) for every element in order; stops when fn returns true.
        template <typename Fn>
        bool forEachTrieElement(const Trie* t, Fn fn) {
            if (t->tree && walkLeaves(t->levels, t->tree, fn)) return true;
            for (unsigned i = 0; i < t->tailLen; ++i) {
                if (fn(static_cast<const ProtoObject*>(t->tail->slots[i]))) return true;
            }
            return false;
        }

        // A trie holding items[0..n), n > MAX_INLINE, built bottom up: full
        // leaves, then each inner level grouping the one below six at a
        // time, then the last 1..6 elements as the tail.  The same shape a
        // run of appendLast produces, for about n / 5 cells.
        const Trie* trieFromArray(ProtoContext* context, const ProtoObject* const* items, proto_ulong n) {
            const proto_ulong tailLen = n - ((n - 1) / TW) * TW;     // 1..6
            const proto_ulong off = n - tailLen;
            const TrieNode* tail = new (context) TrieNode(
                context, static_cast<unsigned>(tailLen), reinterpret_cast<const void* const*>(items + off));
            if (off == 0) {
                return new (context) Trie(context, n, nullptr, tail, 0, static_cast<unsigned>(tailLen));
            }
            std::vector<const void*> level;
            level.reserve(off / TW);
            for (proto_ulong i = 0; i < off; i += TW) {
                level.push_back(new (context) TrieNode(context, TW, reinterpret_cast<const void* const*>(items + i)));
            }
            unsigned levels = 0;
            while (level.size() > 1) {
                std::vector<const void*> up;
                up.reserve((level.size() + TW - 1) / TW);
                for (size_t i = 0; i < level.size(); i += TW) {
                    const unsigned k = static_cast<unsigned>(std::min<size_t>(TW, level.size() - i));
                    up.push_back(new (context) TrieNode(context, k, level.data() + i));
                }
                level.swap(up);
                ++levels;
            }
            return new (context) Trie(context, n, static_cast<const TrieNode*>(level[0]), tail,
                                      levels, static_cast<unsigned>(tailLen));
        }
    }

    const ProtoListTrieNode* ProtoListTrieImplementation::leafFor(proto_ulong index) const {
        if (index >= tailOffset()) return tail;
        unsigned d[kMaxTrieDigits];
        trieDigits(index, levels, d);
        const TrieNode* node = tree;
        for (unsigned k = levels; k > 0; --k) node = childAt(node, d[k]);
        return node;
    }

    const ProtoObject* ProtoListTrieImplementation::implGetAt(ProtoContext*, proto_ulong index) const {
        if (index >= size) return nullptr;
        return static_cast<const ProtoObject*>(leafFor(index)->slots[index % TW]);
    }

    bool ProtoListTrieImplementation::implHas(ProtoContext* context, const ProtoObject* targetValue) const {
        if (!targetValue) return false;
        return forEachTrieElement(this, [&](const ProtoObject* e) {
            return listElementEquals(context, e, targetValue);
        });
    }

    const ProtoListTrieImplementation* ProtoListTrieImplementation::implAppendLast(
        ProtoContext* context, const ProtoObject* value) const
    {
        if (tailLen < TW) {
            const void* s[TW];
            for (unsigned i = 0; i < tailLen; ++i) s[i] = tail->slots[i];
            s[tailLen] = value;
            const TrieNode* newTail = new (context) TrieNode(context, tailLen + 1u, s);
            return new (context) Trie(context, size + 1, tree, newTail, levels, tailLen + 1u);
        }
        // The tail is full: it becomes a leaf of the tree, and the new
        // element starts a new tail.
        const proto_ulong off = tailOffset();
        const TrieNode* newTree;
        unsigned newLevels = levels;
        if (!tree) {
            newTree = tail;
        } else if (off == trieCapacity(levels)) {
            const void* s[2] = {tree, newPath(context, levels, tail)};
            newTree = new (context) TrieNode(context, 2, s);
            newLevels = levels + 1u;
        } else {
            unsigned d[kMaxTrieDigits];
            trieDigits(off, levels, d);
            newTree = pushTail(context, levels, tree, d, tail);
        }
        const void* one[1] = {value};
        const TrieNode* newTail = new (context) TrieNode(context, 1, one);
        return new (context) Trie(context, size + 1, newTree, newTail, newLevels, 1);
    }

    const ProtoListTrieImplementation* ProtoListTrieImplementation::implSetAt(
        ProtoContext* context, proto_ulong index, const ProtoObject* value) const
    {
        if (index >= size) return this;
        const proto_ulong off = tailOffset();
        if (index >= off) {
            const void* s[TW];
            for (unsigned i = 0; i < TW; ++i) s[i] = tail->slots[i];
            s[index - off] = value;
            const TrieNode* newTail = new (context) TrieNode(context, tailLen, s);
            return new (context) Trie(context, size, tree, newTail, levels, tailLen);
        }
        unsigned d[kMaxTrieDigits];
        trieDigits(index, levels, d);
        return new (context) Trie(context, size, setInTree(context, levels, tree, d, value),
                                  tail, levels, tailLen);
    }

    const ProtoListTrieImplementation* ProtoListTrieImplementation::implRemoveLast(ProtoContext* context) const {
        if (tailLen > 1) {
            const TrieNode* newTail = new (context) TrieNode(
                context, tailLen - 1u, tail->slots);
            return new (context) Trie(context, size - 1, tree, newTail, levels, tailLen - 1u);
        }
        // The tail held one element: the tree's last leaf (shared, never
        // copied) becomes the tail.
        const proto_ulong lastLeafStart = tailOffset() - TW;
        const TrieNode* newTail = leafFor(lastLeafStart);
        const TrieNode* newTree = nullptr;
        unsigned newLevels = 0;
        if (levels > 0) {
            unsigned d[kMaxTrieDigits];
            trieDigits(lastLeafStart, levels, d);
            newTree = popTail(context, levels, tree, d);
            newLevels = levels;
            // A root left with a single child gives up one level.
            if (newTree && !newTree->slots[1]) {
                newTree = childAt(newTree, 0);
                --newLevels;
            }
        }
        return new (context) Trie(context, size - 1, newTree, newTail, newLevels, TW);
    }

    proto_ulong ProtoListTrieImplementation::toBuffer(const ProtoObject** out) const {
        proto_ulong at = 0;
        forEachTrieElement(this, [&](const ProtoObject* e) { out[at++] = e; return false; });
        return at;
    }

    const ProtoObject* ProtoListTrieImplementation::implAsObject(ProtoContext*) const {
        ProtoObjectPointer p{};
        p.oid = reinterpret_cast<const ProtoObject*>(this);
        p.op.pointer_tag = POINTER_TAG_LIST;
        return p.oid;
    }

    const ProtoList* ProtoListTrieImplementation::asProtoList(ProtoContext* context) const {
        return reinterpret_cast<const ProtoList*>(implAsObject(context));
    }

    void ProtoListTrieImplementation::processReferences(
        ProtoContext* context, void* self,
        void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (tree) method(context, self, tree);
        if (tail) method(context, self, tail);
    }

    //=========================================================================
    // Output-form selection helpers (file-local)
    //=========================================================================
    namespace {
        // Bottom-up balanced AVL build from a flat array. Cheaper than
        // newList() + N appendLast: skips per-element rebalance walks.
        // Returns nullptr-equivalent (empty list cell) when n == 0.
        const ProtoListImplementation* buildBalancedFromArray(
            ProtoContext* context, const ProtoObject* const* items, unsigned n)
        {
            if (n == 0) {
                return new (context) ProtoListImplementation(context, nullptr, true, nullptr, nullptr);
            }
            unsigned mid = n / 2;
            const ProtoListImplementation* left =
                (mid > 0) ? buildBalancedFromArray(context, items, mid) : nullptr;
            const ProtoListImplementation* right =
                (mid + 1 < n) ? buildBalancedFromArray(context, items + mid + 1, n - mid - 1) : nullptr;
            return new (context) ProtoListImplementation(
                context, items[mid], false,
                (left && left->isEmpty) ? nullptr : left,
                (right && right->isEmpty) ? nullptr : right);
        }

        // Picks the inline form when the resulting size fits, the trie
        // otherwise.  Used by every mutator output path so SmallList stays
        // small as long as possible without forcing a one-way upgrade; a
        // SmallList that outgrows its cell becomes a trie.
        const ProtoList* makeOutputList(
            ProtoContext* context, const ProtoObject* const* items, unsigned n)
        {
            if (n <= ProtoListSmallImplementation::MAX_INLINE) {
                return (new (context) ProtoListSmallImplementation(context, n, items))
                    ->asProtoList(context);
            }
            return trieFromArray(context, items, n)->asProtoList(context);
        }

        // extend() on a trie appends `other` element by element up to this
        // many elements (about 3 cells each); a longer `other` goes through
        // the AVL join, which costs a conversion of the trie.
        constexpr unsigned kTrieExtendByAppend = 64;

        // The cell behind a list handle of any form.
        inline const Cell* listCell(const ProtoList* list) {
            return reinterpret_cast<const Cell*>(
                reinterpret_cast<uintptr_t>(list) & ~static_cast<uintptr_t>(0x3F));
        }

        // The trie behind `list`, or nullptr when the list is in another
        // form.  The trie and the AVL form share POINTER_TAG_LIST (the
        // runtimes recognise a list by its tag), so the cell type tells
        // them apart, as for ProtoMap's two forms.
        inline const Trie* asTrie(const ProtoList* list) {
            ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(list);
            if (pa.op.pointer_tag != POINTER_TAG_LIST) return nullptr;
            const Cell* c = listCell(list);
            return c->getType() == CellType::ListTrie ? static_cast<const Trie*>(c) : nullptr;
        }

        // The AVL tree holding a trie's elements (the structural operations
        // run on the AVL form).  O(n): one cell per element.
        const ProtoListImplementation* trieToTree(ProtoContext* context, const Trie* t) {
            std::vector<const ProtoObject*> buf(t->size);
            t->toBuffer(buf.data());
            return buildBalancedFromArray(context, buf.data(), static_cast<unsigned>(t->size));
        }
    }

    //=========================================================================
    // Structural operations on the AVL form (file-local)
    //=========================================================================
    //
    // The classic persistent-AVL join/split algebra (Blelloch, Ferizovic and
    // Sun, "Just Join for Parallel Ordered Sets", SPAA 2016), specialised to a
    // positional sequence: the in-order traversal is the list, so `join` needs
    // no key comparison.  Every operation path-copies: an existing node is
    // never written, only shared, so every list that reached the caller stays
    // exactly as it was.
    //
    // Inside this block a tree is a `const ProtoListImplementation*` whose
    // nodes are all non-empty; nullptr is the empty tree.  The isEmpty root
    // cell that an AVL list may carry is mapped to nullptr on the way in
    // (treeOf) and never produced on the way out (outputFromTree).
    //
    // Costs, with n and m the sizes of the operands:
    //   join(l, v, r)  O(|h(l) - h(r)| + 1) cells and time
    //   join2(l, r)    O(log n + log m)
    //   take(t, k)     O(log n)  -- the left half of split(t, k)
    //   drop(t, k)     O(log n)  -- the right half of split(t, k)
    // take and drop are the two halves of the usual split(t, k) -> (l, r);
    // they are kept separate because no caller needs both halves of the same
    // split, and building the unused half would only allocate garbage.
    //
    // GC: the intermediate trees live only in C++ locals.  Every public entry
    // point that calls into this block holds a ProtoContext::CriticalSection,
    // the same discipline as the single-element mutators above.
    namespace {
        using ListNode = ProtoListImplementation;

        inline const ListNode* mkNode(ProtoContext* context, const ListNode* left,
                                      const ProtoObject* value, const ListNode* right) {
            return new (context) ListNode(context, value, false, left, right);
        }

        // `left` is taller than `right` by at least 2.  Descends the right
        // spine of `left` to the first subtree no more than one level taller
        // than `right`, joins there and repairs balance on the way up.
        const ListNode* joinRight(ProtoContext* context, const ListNode* left,
                                  const ProtoObject* value, const ListNode* right) {
            const ListNode* l = left->previousNode;
            const ListNode* c = left->nextNode;
            if (get_node_height(c) <= get_node_height(right) + 1) {
                const ListNode* t = mkNode(context, c, value, right);
                if (get_node_height(t) <= get_node_height(l) + 1) {
                    return mkNode(context, l, left->value, t);
                }
                return leftRotate(context, mkNode(context, l, left->value, rightRotate(context, t)));
            }
            const ListNode* t = joinRight(context, c, value, right);
            const ListNode* t2 = mkNode(context, l, left->value, t);
            if (get_node_height(t) <= get_node_height(l) + 1) return t2;
            return leftRotate(context, t2);
        }

        // Mirror image of joinRight: `right` is taller than `left` by at
        // least 2.
        const ListNode* joinLeft(ProtoContext* context, const ListNode* left,
                                 const ProtoObject* value, const ListNode* right) {
            const ListNode* c = right->previousNode;
            const ListNode* r = right->nextNode;
            if (get_node_height(c) <= get_node_height(left) + 1) {
                const ListNode* t = mkNode(context, left, value, c);
                if (get_node_height(t) <= get_node_height(r) + 1) {
                    return mkNode(context, t, right->value, r);
                }
                return rightRotate(context, mkNode(context, leftRotate(context, t), right->value, r));
            }
            const ListNode* t = joinLeft(context, left, value, c);
            const ListNode* t2 = mkNode(context, t, right->value, r);
            if (get_node_height(t) <= get_node_height(r) + 1) return t2;
            return rightRotate(context, t2);
        }

        // The balanced tree whose in-order sequence is left ++ [value] ++ right.
        const ListNode* join(ProtoContext* context, const ListNode* left,
                             const ProtoObject* value, const ListNode* right) {
            const int hl = get_node_height(left);
            const int hr = get_node_height(right);
            if (hl > hr + 1) return joinRight(context, left, value, right);
            if (hr > hl + 1) return joinLeft(context, left, value, right);
            return mkNode(context, left, value, right);
        }

        // `t` without its last element, which is stored in *last.  t != nullptr.
        const ListNode* popLast(ProtoContext* context, const ListNode* t,
                                const ProtoObject** last) {
            if (!t->nextNode) {
                *last = t->value;
                return t->previousNode;
            }
            const ListNode* rest = popLast(context, t->nextNode, last);
            return join(context, t->previousNode, t->value, rest);
        }

        // `t` without its first element, which is stored in *first.  t != nullptr.
        const ListNode* popFirst(ProtoContext* context, const ListNode* t,
                                 const ProtoObject** first) {
            if (!t->previousNode) {
                *first = t->value;
                return t->nextNode;
            }
            const ListNode* rest = popFirst(context, t->previousNode, first);
            return join(context, rest, t->value, t->nextNode);
        }

        // Concatenation: the balanced tree whose sequence is left ++ right.
        // The middle element of the join is taken from the shorter tree, so
        // the pop costs O(height of the shorter tree) and the join
        // O(height difference): extending a large list by one element costs
        // about what one appendLast costs.
        const ListNode* join2(ProtoContext* context, const ListNode* left,
                              const ListNode* right) {
            if (!left) return right;
            if (!right) return left;
            const ProtoObject* middle = nullptr;
            if (get_node_height(left) >= get_node_height(right)) {
                const ListNode* rest = popFirst(context, right, &middle);
                return join(context, left, middle, rest);
            }
            const ListNode* rest = popLast(context, left, &middle);
            return join(context, rest, middle, right);
        }

        // The first k elements of t (left half of split(t, k)).
        const ListNode* take(ProtoContext* context, const ListNode* t, proto_ulong k) {
            if (!t || k == 0) return nullptr;
            if (k >= t->size) return t;
            const proto_ulong ls = get_node_size(t->previousNode);
            if (k <= ls) return take(context, t->previousNode, k);
            return join(context, t->previousNode, t->value,
                        take(context, t->nextNode, k - ls - 1));
        }

        // Every element of t after the first k (right half of split(t, k)).
        const ListNode* drop(ProtoContext* context, const ListNode* t, proto_ulong k) {
            if (!t || k >= t->size) return nullptr;
            if (k == 0) return t;
            const proto_ulong ls = get_node_size(t->previousNode);
            if (k > ls) return drop(context, t->nextNode, k - ls - 1);
            return join(context, drop(context, t->previousNode, k), t->value, t->nextNode);
        }

        // In-order copy of a tree into `out`, starting at `at`; returns the
        // index past the last element written.  The caller sizes `out`.
        unsigned treeToBuffer(const ListNode* t, const ProtoObject** out, unsigned at) {
            if (!t) return at;
            at = treeToBuffer(t->previousNode, out, at);
            out[at++] = t->value;
            return treeToBuffer(t->nextNode, out, at);
        }

        // A list in the tree vocabulary of this block.  A SmallList becomes a
        // freshly built balanced tree of at most MAX_INLINE nodes.
        const ListNode* treeOf(ProtoContext* context, const ProtoList* list) {
            ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(list);
            if (const Trie* trie = asTrie(list)) return trieToTree(context, trie);
            if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
                const auto* small = toImpl<const ProtoListSmallImplementation>(list);
                if (small->size == 0) return nullptr;
                return buildBalancedFromArray(context, small->slots,
                                              static_cast<unsigned>(small->size));
            }
            const auto* avl = toImpl<const ListNode>(list);
            return (avl->isEmpty || avl->size == 0) ? nullptr : avl;
        }

        // Appends the stored elements of a list of at most MAX_INLINE elements
        // to `out` at `at`, as stored: unlike ProtoList::getAt, a nullptr
        // element is not turned into PROTO_NONE.  Returns the new end.
        unsigned rawElementsToBuffer(ProtoContext* context, const ProtoList* list,
                                     const ProtoObject** out, unsigned at) {
            ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(list);
            if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
                const auto* small = toImpl<const ProtoListSmallImplementation>(list);
                for (proto_ulong i = 0; i < small->size; ++i) out[at++] = small->slots[i];
                return at;
            }
            if (const Trie* trie = asTrie(list)) return at + static_cast<unsigned>(trie->toBuffer(out + at));
            const auto* avl = toImpl<const ListNode>(list);
            if (avl->isEmpty) return at;
            return treeToBuffer(avl, out, at);
        }

        // The output form of a result tree, chosen by size like every other
        // mutator: at most MAX_INLINE elements -> SmallList, more -> AVL.
        const ProtoList* outputFromTree(ProtoContext* context, const ListNode* t) {
            const proto_ulong n = get_node_size(t);
            if (n <= ProtoListSmallImplementation::MAX_INLINE) {
                const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE];
                const unsigned count = treeToBuffer(t, buf, 0);
                return makeOutputList(context, buf, count);
            }
            return t->asProtoList(context);
        }

        // Clamp a [from, to) range into [0, size].  The comparison is done in
        // 64 bits, so a size beyond INT_MAX is never narrowed; the clamped
        // values fit in an int because each is at most its own non-negative
        // input.
        void clampRange(proto_ulong size, int& from, int& to) {
            const long long s = size > static_cast<proto_ulong>(LLONG_MAX)
                ? LLONG_MAX : static_cast<long long>(size);
            const long long f = std::clamp(static_cast<long long>(from), 0LL, s);
            const long long t = std::clamp(static_cast<long long>(to), 0LL, s);
            from = static_cast<int>(f);
            to = static_cast<int>(t);
        }

        // The output form of a tree mutator's result, chosen by size: an AVL
        // result of at most MAX_INLINE elements becomes a SmallList.  Accepts
        // the isEmpty root cell an AVL list may carry.
        const ProtoList* outputFromImpl(ProtoContext* context, const ListNode* impl) {
            if (impl->isEmpty || impl->size == 0) return context->newList();
            return outputFromTree(context, impl);
        }
    }

    //=========================================================================
    // ProtoListIteratorImplementation
    //=========================================================================

    ProtoListIteratorImplementation::ProtoListIteratorImplementation(
        ProtoContext* context,
        const ProtoObject* b,
        proto_ulong index,
        const ProtoListTrieNode* l,
        proto_ulong lStart
    ) : Cell(context), base(b), currentIndex(index), leaf(l), leafStart(lStart)
    {
    }

    namespace {
        // Iterator helpers — read size / element from the tagged base
        // pointer without a virtual call.
        proto_ulong iterBaseSize(const ProtoObject* base) {
            if (!base) return 0;
            ProtoObjectPointer pa{}; pa.oid = base;
            if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
                return toImpl<const ProtoListSmallImplementation>(base)->size;
            }
            return toImpl<const ProtoListImplementation>(base)->size;
        }
        const ProtoObject* iterBaseGetAt(ProtoContext* context, const ProtoObject* base, proto_ulong index) {
            if (!base) return nullptr;
            ProtoObjectPointer pa{}; pa.oid = base;
            if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
                return toImpl<const ProtoListSmallImplementation>(base)
                    ->implGetAt(context, static_cast<int>(index));
            }
            return toImpl<const ProtoListImplementation>(base)
                ->implGetAt(context, static_cast<int>(index));
        }
    }

    // An iterator over a trie carries the leaf holding currentIndex (a trie
    // is never empty, so that leaf always exists); `leaf != nullptr` is what
    // marks a trie base, so the AVL and inline paths stay as they were.
    namespace {
        inline const ProtoListTrieImplementation* iterTrie(const ProtoObject* base) {
            return reinterpret_cast<const ProtoListTrieImplementation*>(
                reinterpret_cast<uintptr_t>(base) & ~static_cast<uintptr_t>(0x3F));
        }
    }

    int ProtoListIteratorImplementation::implHasNext() const {
        if (!this->base) return false;
        if (this->leaf) return this->currentIndex < iterTrie(this->base)->size;
        return this->currentIndex < iterBaseSize(this->base);
    }

    const ProtoObject* ProtoListIteratorImplementation::implNext(ProtoContext* context) const {
        if (this->leaf) {
            if (this->currentIndex >= iterTrie(this->base)->size) return nullptr;
            return static_cast<const ProtoObject*>(this->leaf->slots[this->currentIndex - this->leafStart]);
        }
        return iterBaseGetAt(context, this->base, this->currentIndex);
    }

    const ProtoListIteratorImplementation* ProtoListIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (this->leaf) {
            const auto* trie = iterTrie(this->base);
            if (this->currentIndex >= trie->size) return this;
            const proto_ulong next = this->currentIndex + 1;
            if (next < this->leafStart + ProtoListTrieNode::WIDTH || next >= trie->size) {
                return new (context) ProtoListIteratorImplementation(
                    context, this->base, next, this->leaf, this->leafStart);
            }
            return new (context) ProtoListIteratorImplementation(
                context, this->base, next, trie->leafFor(next), next);
        }
        if (this->currentIndex < iterBaseSize(this->base)) {
            return new (context) ProtoListIteratorImplementation(context, this->base, this->currentIndex + 1);
        }
        return this;
    }

    const ProtoListIterator* ProtoListIteratorImplementation::asProtoListIterator(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.listIterator;
    }

    const ProtoObject* ProtoListIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.listIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_LIST_ITERATOR;
        return p.oid;
    }

    void ProtoListIteratorImplementation::processReferences(ProtoContext* context, void* callback_data, void (*callback)(ProtoContext*, void*, const Cell*)) const {
        if (const Cell* c = ProtoObject::asCellPointer(base)) {
            callback(context, callback_data, c);
        }
        // Reachable from base too; reported so the iterator never depends on that.
        if (leaf) callback(context, callback_data, leaf);
    }

    // ProtoList / ProtoListIterator external API trampolines
    //
    // Each method dispatches on POINTER_TAG_{LIST,LIST_SMALL} once at entry.
    // Read-only methods read directly from the relevant impl form (no
    // promotion). Mutator methods produce a fresh output whose form is
    // selected by the resulting size: ≤ MAX_INLINE → SmallList, > MAX_INLINE
    // → AVL. Only `appendLast` overflowing past MAX_INLINE triggers a
    // strict promotion (the result genuinely cannot fit inline).

    // GC critical sections on the immutable-collection mutator API.
    //
    // Every mutator builds a fresh tree of cells; the result is held in a
    // C++ local until the caller publishes it.  Without the guard a
    // concurrent STW root scan landing between two allocations would
    // observe the half-built tree as candidate-but-unreachable and a
    // sweep would free it.  Same discipline as
    // ProtoObject::setAttribute, ProtoMultiset::add, ProtoSet::add.
    //
    // The guard is per-thread, lock-free, single-int; it is cheap to
    // enter and callers that already wrap a CriticalSection (e.g.
    // setAttribute) just bump the depth counter — no nested STW
    // suppression issue.

    namespace {
        // Read a SmallList's slots into a stack-buffer so a mutator can
        // recompute the result.  Returns the count.
        unsigned smallToBuffer(const ProtoListSmallImplementation* small,
                               const ProtoObject** buf) {
            unsigned n = static_cast<unsigned>(small->size);
            for (unsigned i = 0; i < n; ++i) buf[i] = small->slots[i];
            return n;
        }

        // Pull every element of an AVL list into a flat heap vector.  Used
        // by the rare path where a SmallList is mixed with an AVL list in
        // a mutator (e.g. extend) and we want a single-pass output build.
        void avlToVector(ProtoContext* context, const ProtoListImplementation* avl,
                         std::vector<const ProtoObject*>& out) {
            proto_ulong n = avl->size;
            out.reserve(out.size() + n);
            for (proto_ulong i = 0; i < n; ++i) {
                out.push_back(avl->implGetAt(context, static_cast<int>(i)));
            }
        }
    }

    proto_ulong ProtoList::getSize(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            return toImpl<const ProtoListSmallImplementation>(this)->size;
        }
        if (const Trie* trie = asTrie(this)) return trie->size;
        return toImpl<const ProtoListImplementation>(this)->size;
    }
    const ProtoObject* ProtoList::getAt(ProtoContext* context, int index) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        const ProtoObject* result;
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            result = toImpl<const ProtoListSmallImplementation>(this)->implGetAt(context, index);
        } else if (const Trie* trie = asTrie(this)) {
            result = index < 0 ? nullptr : trie->implGetAt(context, static_cast<proto_ulong>(index));
        } else {
            result = toImpl<const ProtoListImplementation>(this)->implGetAt(context, index);
        }
        return result ? result : PROTO_NONE;
    }
    const ProtoObject* ProtoList::getFirst(ProtoContext* context) const {
        proto_ulong size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, 0);
    }
    const ProtoObject* ProtoList::getLast(ProtoContext* context) const {
        proto_ulong size = getSize(context);
        if (size == 0) return PROTO_NONE;
        return getAt(context, toIndex(size - 1));
    }
    bool ProtoList::has(ProtoContext* context, const ProtoObject* value) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            return toImpl<const ProtoListSmallImplementation>(this)->implHas(context, value);
        }
        if (const Trie* trie = asTrie(this)) return trie->implHas(context, value);
        return toImpl<const ProtoListImplementation>(this)->implHas(context, value);
    }
    // --- ProtoArguments ---

    bool ProtoArguments::has(ProtoContext* context, const ProtoObject* value) const {
        if (!value) return false;
        for (unsigned i = 0; i < count_; ++i) {
            if (values_[i] && listElementEquals(context, values_[i], value)) return true;
        }
        return false;
    }

    const ProtoList* ProtoArguments::asList(ProtoContext* context) const {
        if (list_) return list_;
        return count_ ? context->newList(count_, values_) : context->newList();
    }

    ProtoListArguments::ProtoListArguments(ProtoContext* context, const ProtoList* list)
        : ProtoArguments(nullptr, 0, list) {
        if (!list) return;
        ProtoObjectPointer pa{};
        pa.oid = reinterpret_cast<const ProtoObject*>(list);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            // A small list already holds its values in a dense array in its
            // cell, which the list keeps alive: read it in place, no copy.
            const auto* small = toImpl<const ProtoListSmallImplementation>(list);
            values_ = small->slots;
            count_ = static_cast<unsigned>(small->size);
            return;
        }
        const unsigned n = static_cast<unsigned>(list->getSize(context));
        const ProtoObject** slots = inline_;
        if (n > kInline) {
            heap_.resize(n);
            slots = heap_.data();
        }
        for (unsigned i = 0; i < n; ++i) slots[i] = list->getAt(context, static_cast<int>(i));
        values_ = slots;
        count_ = n;
    }

    const ProtoList* ProtoList::setAt(ProtoContext* context, int index, const ProtoObject* value) const {
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            if (index < 0 || static_cast<proto_ulong>(index) >= small->size) {
                return const_cast<ProtoList*>(this);
            }
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE];
            unsigned n = smallToBuffer(small, buf);
            buf[index] = value;
            return makeOutputList(context, buf, n);
        }
        if (const Trie* trie = asTrie(this)) {
            if (index < 0 || static_cast<proto_ulong>(index) >= trie->size) return const_cast<ProtoList*>(this);
            return trie->implSetAt(context, static_cast<proto_ulong>(index), value)->asProtoList(context);
        }
        const auto* impl = toImpl<const ProtoListImplementation>(this);
        const ProtoListImplementation* result = impl->implSetAt(context, index, value);
        if (result == impl) return const_cast<ProtoList*>(this);   // out of range: unchanged
        return outputFromImpl(context, result);
    }
    const ProtoList* ProtoList::insertAt(ProtoContext* context, int index, const ProtoObject* value) const {
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            unsigned n = static_cast<unsigned>(small->size);
            if (index < 0) index = 0;
            if (static_cast<unsigned>(index) > n) index = static_cast<int>(n);
            // Resulting size = n + 1.  If it fits inline, stay SmallList;
            // otherwise build AVL via buildBalancedFromArray.
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE + 1];
            for (unsigned i = 0; i < static_cast<unsigned>(index); ++i) buf[i] = small->slots[i];
            buf[index] = value;
            for (unsigned i = static_cast<unsigned>(index); i < n; ++i) buf[i + 1] = small->slots[i];
            return makeOutputList(context, buf, n + 1);
        }
        if (const Trie* trie = asTrie(this)) {
            // At the end: an append, native to the trie.  Anywhere else the
            // list moves to the AVL form, where inserting is O(log n).
            if (index < 0) index = 0;
            if (static_cast<proto_ulong>(index) >= trie->size) {
                return trie->implAppendLast(context, value)->asProtoList(context);
            }
            return outputFromImpl(context, trieToTree(context, trie)->implInsertAt(context, index, value));
        }
        // Clamp into [0, size] as the small path does: implInsertAt compares
        // the index with unsigned sizes, so a negative one would land at the
        // end of the list instead of the front.
        const auto* impl = toImpl<const ProtoListImplementation>(this);
        const proto_ulong size = impl->isEmpty ? 0 : impl->size;
        if (index < 0) index = 0;
        if (static_cast<proto_ulong>(index) > size) index = static_cast<int>(size);
        return outputFromImpl(context, impl->implInsertAt(context, index, value));
    }
    const ProtoList* ProtoList::appendFirst(ProtoContext* context, const ProtoObject* value) const { return insertAt(context, 0, value); }
    const ProtoList* ProtoList::appendLast(ProtoContext* context, const ProtoObject* value) const {
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE + 1];
            unsigned n = smallToBuffer(small, buf);
            buf[n] = value;
            return makeOutputList(context, buf, n + 1);
        }
        if (const Trie* trie = asTrie(this)) {
            return trie->implAppendLast(context, value)->asProtoList(context);
        }
        return outputFromImpl(context,
            toImpl<const ProtoListImplementation>(this)->implAppendLast(context, value));
    }
    const ProtoList* ProtoList::extend(ProtoContext* context, const ProtoList* other) const {
        // Concatenation: O(log n + log m) cells and time, whatever the sizes.
        // Elements are copied as stored (a nullptr element stays nullptr).
        // The guard keeps the intermediate trees rooted.
        if (!other) return const_cast<ProtoList*>(this);
        const proto_ulong otherSize = other->getSize(context);
        if (otherSize == 0) return const_cast<ProtoList*>(this);
        ProtoContext::CriticalSection cs(context);
        const proto_ulong selfSize = getSize(context);
        if (selfSize + otherSize <= ProtoListSmallImplementation::MAX_INLINE) {
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE];
            unsigned n = rawElementsToBuffer(context, this, buf, 0);
            n = rawElementsToBuffer(context, other, buf, n);
            return makeOutputList(context, buf, n);
        }
        // A trie extended by a short list: appends, native to the trie and
        // cheaper than a conversion to the AVL form plus a join.
        if (const Trie* trie = asTrie(this)) {
            if (otherSize <= kTrieExtendByAppend) {
                const ProtoObject* buf[kTrieExtendByAppend];
                const unsigned k = rawElementsToBuffer(context, other, buf, 0);
                for (unsigned i = 0; i < k; ++i) trie = trie->implAppendLast(context, buf[i]);
                return trie->asProtoList(context);
            }
        }
        // A SmallList operand next to a tree: join around one of its own
        // elements, so the small side costs at most MAX_INLINE - 1 cells and
        // the join only the height difference.
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        ProtoObjectPointer pb{}; pb.oid = reinterpret_cast<const ProtoObject*>(other);
        if (pb.op.pointer_tag == POINTER_TAG_LIST_SMALL && pa.op.pointer_tag != POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(other);
            const unsigned k = static_cast<unsigned>(small->size);
            const ProtoListImplementation* rest =
                k > 1 ? buildBalancedFromArray(context, small->slots + 1, k - 1) : nullptr;
            return outputFromTree(context, join(context, treeOf(context, this), small->slots[0], rest));
        }
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL && pb.op.pointer_tag != POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            const unsigned k = static_cast<unsigned>(small->size);
            if (k == 0) return outputFromTree(context, treeOf(context, other));
            const ProtoListImplementation* head =
                k > 1 ? buildBalancedFromArray(context, small->slots, k - 1) : nullptr;
            return outputFromTree(context, join(context, head, small->slots[k - 1], treeOf(context, other)));
        }
        return outputFromTree(context, join2(context, treeOf(context, this), treeOf(context, other)));
    }
    const ProtoList* ProtoList::splitFirst(ProtoContext* context, int index) const {
        // The first `index` elements; index <= 0 -> empty, index >= size -> this.
        proto_ulong size = getSize(context);
        if (index <= 0) return context->newList();
        if (static_cast<proto_ulong>(index) >= size) return const_cast<ProtoList*>(this);
        return getSlice(context, 0, index);
    }
    const ProtoList* ProtoList::splitLast(ProtoContext* context, int index) const {
        // The last `index` elements; index <= 0 -> empty, index >= size -> this.
        proto_ulong size = getSize(context);
        if (index <= 0) return context->newList();
        if (static_cast<proto_ulong>(index) >= size) return const_cast<ProtoList*>(this);
        return getSlice(context, toIndex(size - index), toIndex(size));
    }
    const ProtoList* ProtoList::removeFirst(ProtoContext* context) const {
        proto_ulong size = getSize(context);
        if (size == 0) return const_cast<ProtoList*>(this);
        return removeAt(context, 0);
    }
    const ProtoList* ProtoList::removeLast(ProtoContext* context) const {
        proto_ulong size = getSize(context);
        if (size == 0) return const_cast<ProtoList*>(this);
        return removeAt(context, toIndex(size - 1));
    }
    const ProtoList* ProtoList::removeAt(ProtoContext* context, int index) const {
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            unsigned n = static_cast<unsigned>(small->size);
            if (index < 0 || static_cast<unsigned>(index) >= n) {
                return const_cast<ProtoList*>(this);
            }
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE];
            unsigned out = 0;
            for (unsigned i = 0; i < n; ++i) {
                if (i == static_cast<unsigned>(index)) continue;
                buf[out++] = small->slots[i];
            }
            return makeOutputList(context, buf, out);
        }
        if (const Trie* trie = asTrie(this)) {
            if (index < 0 || static_cast<proto_ulong>(index) >= trie->size) return const_cast<ProtoList*>(this);
            if (static_cast<proto_ulong>(index) == trie->size - 1) {
                // removeLast, native to the trie; a result that fits inline
                // becomes a SmallList, like every other mutator's.
                if (trie->size - 1 <= ProtoListSmallImplementation::MAX_INLINE) {
                    const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE + 1];
                    trie->toBuffer(buf);
                    return makeOutputList(context, buf, static_cast<unsigned>(trie->size - 1));
                }
                return trie->implRemoveLast(context)->asProtoList(context);
            }
            return outputFromImpl(context, trieToTree(context, trie)->implRemoveAt(context, index));
        }
        const auto* impl = toImpl<const ProtoListImplementation>(this);
        const ProtoListImplementation* result = impl->implRemoveAt(context, index);
        if (result == impl) return const_cast<ProtoList*>(this);   // out of range: unchanged
        return outputFromImpl(context, result);
    }
    const ProtoList* ProtoList::removeSlice(ProtoContext* context, int from, int to) const {
        // Removes the elements at [from, to).  The range is clamped into
        // [0, size]; an empty range (from >= to after clamping) returns this
        // list unchanged.  O(log n): join2(take(from), drop(to)).
        const proto_ulong size = getSize(context);
        clampRange(size, from, to);
        if (from >= to) return const_cast<ProtoList*>(this);
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE];
            unsigned n = 0;
            for (unsigned i = 0; i < static_cast<unsigned>(small->size); ++i) {
                if (static_cast<int>(i) >= from && static_cast<int>(i) < to) continue;
                buf[n++] = small->slots[i];
            }
            return makeOutputList(context, buf, n);
        }
        const ProtoListImplementation* t = treeOf(context, this);
        return outputFromTree(context,
            join2(context, take(context, t, static_cast<proto_ulong>(from)),
                           drop(context, t, static_cast<proto_ulong>(to))));
    }
    const ProtoList* ProtoList::getSlice(ProtoContext* context, int from, int to) const {
        // The elements at [from, to).  The range is clamped into [0, size]:
        // a negative `from` counts as 0 and a `to` past the end as size (no
        // Python-style negative indexing).  An empty range (from >= to after
        // clamping) yields an empty list; the full range yields this list.
        // O(log n) cells and time, independent of the slice length.
        const proto_ulong size = getSize(context);
        clampRange(size, from, to);
        if (from >= to) return context->newList();
        if (from == 0 && static_cast<proto_ulong>(to) == size) return const_cast<ProtoList*>(this);
        ProtoContext::CriticalSection cs(context);
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            const auto* small = toImpl<const ProtoListSmallImplementation>(this);
            return makeOutputList(context, small->slots + from, static_cast<unsigned>(to - from));
        }
        const ProtoListImplementation* t = treeOf(context, this);
        const ProtoListImplementation* tail = drop(context, t, static_cast<proto_ulong>(from));
        return outputFromTree(context, take(context, tail, static_cast<proto_ulong>(to - from)));
    }
    const ProtoObject* ProtoList::asObject(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            return toImpl<const ProtoListSmallImplementation>(this)->implAsObject(context);
        }
        if (const Trie* trie = asTrie(this)) return trie->implAsObject(context);
        return toImpl<const ProtoListImplementation>(this)->implAsObject(context);
    }
    const ProtoListIterator* ProtoList::getIterator(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            // Use the tagged pointer (`this`) directly as the iterator base.
            return (new (context) ProtoListIteratorImplementation(
                        context, reinterpret_cast<const ProtoObject*>(this), 0))
                    ->asProtoListIterator(context);
        }
        if (const Trie* trie = asTrie(this)) {
            return (new (context) ProtoListIteratorImplementation(
                        context, reinterpret_cast<const ProtoObject*>(this), 0, trie->leafFor(0), 0))
                    ->asProtoListIterator(context);
        }
        return toImpl<const ProtoListImplementation>(this)->implGetIterator(context)->asProtoListIterator(context);
    }
    proto_ulong ProtoList::getHash(ProtoContext* context) const {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_LIST_SMALL) {
            return toImpl<const ProtoListSmallImplementation>(this)->getHash(context);
        }
        return listCell(this)->getHash(context);
    }

    int ProtoListIterator::hasNext(ProtoContext* context) const { if (!this) return 0; return toImpl<const ProtoListIteratorImplementation>(this)->implHasNext(); }
    const ProtoObject* ProtoListIterator::next(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoListIteratorImplementation>(this)->implNext(context); }
    const ProtoListIterator* ProtoListIterator::advance(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoListIteratorImplementation>(this)->implAdvance(context)->asProtoListIterator(context); }
    const ProtoObject* ProtoListIterator::asObject(ProtoContext* context) const { if (!this) return nullptr; return toImpl<const ProtoListIteratorImplementation>(this)->implAsObject(context); }

    const ProtoList* ProtoList::multiply(ProtoContext* context, const ProtoObject* count) const {
        // The receiver repeated `count` times, built by binary doubling over
        // join2: O(log count) concatenations of O(log n) each, sharing the
        // receiver's nodes, instead of n * count appends.  Elements are
        // copied as stored (a nullptr element stays nullptr).
        if (!count->isInteger(context)) return nullptr;
        long long n = count->asLong(context);
        if (n <= 0) return context->newList();
        if (n == 1) return const_cast<ProtoList*>(this);
        const proto_ulong selfSize = getSize(context);
        if (selfSize == 0) return context->newList();
        if (static_cast<proto_ulong>(n) > ~static_cast<proto_ulong>(0) / selfSize)
            context->raiseError(ProtoErrorKind::Overflow, "List repetition count too large.");

        ProtoContext::CriticalSection cs(context);
        if (selfSize * static_cast<proto_ulong>(n) <= ProtoListSmallImplementation::MAX_INLINE) {
            const ProtoObject* once[ProtoListSmallImplementation::MAX_INLINE];
            const unsigned k = rawElementsToBuffer(context, this, once, 0);
            const ProtoObject* buf[ProtoListSmallImplementation::MAX_INLINE];
            unsigned out = 0;
            for (long long r = 0; r < n; ++r)
                for (unsigned i = 0; i < k; ++i) buf[out++] = once[i];
            return makeOutputList(context, buf, out);
        }
        const ProtoListImplementation* base = treeOf(context, this);
        const ProtoListImplementation* acc = nullptr;
        unsigned long long k = static_cast<unsigned long long>(n);
        while (k) {
            if (k & 1ULL) acc = join2(context, acc, base);
            k >>= 1;
            if (k) base = join2(context, base, base);
        }
        return outputFromTree(context, acc);
    }
}
