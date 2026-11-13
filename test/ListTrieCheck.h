// ListTrieCheck.h — structural invariants of ProtoList's trie form, shared by
// the list tests.  An empty string means the trie is well formed.
#pragma once

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <string>

namespace proto_test {

using proto::Cell;
using proto::CellType;
using proto::ProtoList;
using proto::ProtoListTrieImplementation;
using proto::ProtoListTrieNode;
using proto::ProtoObjectPointer;
using proto::proto_ulong;

// The trie behind a list handle, or nullptr when the list is in another form.
inline const ProtoListTrieImplementation* trieOf(const ProtoList* l) {
    ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const proto::ProtoObject*>(l);
    if (pa.op.pointer_tag != POINTER_TAG_LIST) return nullptr;
    const Cell* c = reinterpret_cast<const Cell*>(reinterpret_cast<uintptr_t>(l) & ~static_cast<uintptr_t>(0x3F));
    return c->getType() == CellType::ListTrie ? static_cast<const ProtoListTrieImplementation*>(c) : nullptr;
}

inline bool isTrie(const ProtoList* l) { return trieOf(l) != nullptr; }

namespace detail {
inline proto_ulong pow6(unsigned k) { proto_ulong p = 1; while (k--) p *= 6; return p; }

// Leaves under `node` at `level`; -1 after recording a failure.  Inner nodes
// are packed to the left and every child but the last is full.
inline long long countLeaves(unsigned level, const ProtoListTrieNode* node, std::string& why) {
    if (!node) { why = "null node inside the tree"; return -1; }
    if (node->getType() != CellType::ListTrieNode) { why = "tree slot is not a trie node"; return -1; }
    if (level == 0) return 1;
    long long total = 0;
    unsigned children = 0;
    for (unsigned i = 0; i < ProtoListTrieNode::WIDTH; ++i) {
        if (!node->slots[i]) {
            for (unsigned j = i + 1; j < ProtoListTrieNode::WIDTH; ++j)
                if (node->slots[j]) { why = "inner node not packed to the left"; return -1; }
            break;
        }
        ++children;
    }
    if (children == 0) { why = "inner node without children"; return -1; }
    for (unsigned i = 0; i < children; ++i) {
        const long long n = countLeaves(level - 1, static_cast<const ProtoListTrieNode*>(node->slots[i]), why);
        if (n < 0) return -1;
        if (i + 1 < children && static_cast<proto_ulong>(n) != pow6(level - 1)) {
            why = "a child other than the last is not full"; return -1;
        }
        total += n;
    }
    return total;
}
}  // namespace detail

inline std::string checkTrie(const ProtoListTrieImplementation* t) {
    const unsigned W = ProtoListTrieNode::WIDTH;
    if (t->size <= proto::ProtoListSmallImplementation::MAX_INLINE) return "trie no larger than MAX_INLINE";
    if (t->tailLen < 1 || t->tailLen > W) return "tailLen out of 1..6";
    const proto_ulong off = t->size - t->tailLen;
    if (off != ((t->size - 1) / W) * W) return "tailOffset is not ((size - 1) / 6) * 6";
    if (!t->tail || t->tail->getType() != CellType::ListTrieNode) return "tail is not a trie node";
    for (unsigned i = t->tailLen; i < W; ++i)
        if (t->tail->slots[i]) return "tail slot past tailLen is not nullptr";
    if (off == 0) return t->tree ? "tree present with an empty tree part" : "";
    if (!t->tree) return "elements before the tail but no tree";
    // Minimal height: levels inner levels hold more than 6^levels elements.
    if (off > detail::pow6(t->levels + 1u)) return "tree too short for its elements";
    if (t->levels > 0 && off <= detail::pow6(t->levels)) return "tree taller than needed";
    std::string why;
    const long long leaves = detail::countLeaves(t->levels, t->tree, why);
    if (leaves < 0) return why;
    if (static_cast<proto_ulong>(leaves) * W != off) return "leaf count does not match tailOffset";
    return "";
}

}  // namespace proto_test
