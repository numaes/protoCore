/*
 * SparseListAlgorithms.h — internal to protoCore (never installed).
 *
 * The persistent AVL and Small-form algorithms shared by ProtoSparseList
 * (key: unsigned long) and ProtoSparseListObject (key: const ProtoObject*).
 * Keys are ordered and compared by their machine word, so both types have
 * one algorithmic implementation.
 *
 * Everything lives in an anonymous namespace: each translation unit gets its
 * own internal-linkage instantiation, so calls stay direct (no PLT) and no
 * symbol is added to libprotoCore's exported interface — exactly the linkage
 * the former file-local helpers in ProtoSparseList.cpp had.
 *
 * Callers wrap every mutator in a ProtoContext::CriticalSection.
 */

#ifndef PROTOCORE_SPARSE_LIST_ALGORITHMS_H
#define PROTOCORE_SPARSE_LIST_ALGORITHMS_H

#include "../headers/proto_internal.h"
#include <algorithm>
#include <cstdint>

namespace proto::sparse_avl {
namespace {

    inline uintptr_t keyWord(unsigned long k) { return k; }
    inline uintptr_t keyWord(const ProtoObject* k) { return reinterpret_cast<uintptr_t>(k); }

    template<class Node>
    inline unsigned long nodeSize(const Node* node) {
        if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) return 0;
        return node->size;
    }

    template<class Node>
    inline int nodeHeight(const Node* node) {
        if (!node || (reinterpret_cast<uintptr_t>(node) & 0x3F) != 0) return 0;
        return node->height;
    }

    template<class Node>
    inline int balanceOf(const Node* node) {
        if (!node || node->isEmpty) return 0;
        return sparse_avl::nodeHeight(node->previous) - sparse_avl::nodeHeight(node->next);
    }

    template<class Node>
    inline const Node* makeEmpty(ProtoContext* context) {
        return new(context) Node(context, typename Node::KeyType{}, nullptr, nullptr, nullptr, true);
    }

    template<class Node>
    const Node* rightRotate(ProtoContext* context, const Node* y) {
        const Node* x = y->previous;
        const Node* t2 = x->next;
        auto* newY = new(context) Node(context, y->key, y->value, t2, y->next, false);
        return new(context) Node(context, x->key, x->value, x->previous, newY, false);
    }

    template<class Node>
    const Node* leftRotate(ProtoContext* context, const Node* x) {
        const Node* y = x->next;
        const Node* t2 = y->previous;
        auto* newX = new(context) Node(context, x->key, x->value, x->previous, t2, false);
        return new(context) Node(context, y->key, y->value, newX, y->next, false);
    }

    template<class Node>
    const Node* rebalance(ProtoContext* context, const Node* node) {
        const int balance = sparse_avl::balanceOf(node);
        if (balance > 1) {                                   // left heavy
            if (sparse_avl::balanceOf(node->previous) < 0) {  // left-right
                const Node* newPrev = sparse_avl::leftRotate(context, node->previous);
                return sparse_avl::rightRotate(context, new(context) Node(context, node->key, node->value, newPrev, node->next, false));
            }
            return sparse_avl::rightRotate(context, node);  // left-left
        }
        if (balance < -1) {                                  // right heavy
            if (sparse_avl::balanceOf(node->next) > 0) {  // right-left
                const Node* newNext = sparse_avl::rightRotate(context, node->next);
                return sparse_avl::leftRotate(context, new(context) Node(context, node->key, node->value, node->previous, newNext, false));
            }
            return sparse_avl::leftRotate(context, node);  // right-right
        }
        return node;
    }

    template<class Node>
    const ProtoObject* getAt(const Node* root, typename Node::KeyType key) {
        const uintptr_t w = sparse_avl::keyWord(key);
        const Node* node = root;
        while (node) {
            if (node->isEmpty) break;
            const uintptr_t nk = sparse_avl::keyWord(node->key);
            if (w < nk) node = node->previous;
            else if (w > nk) node = node->next;
            else return node->value;
        }
        return nullptr;
    }

    template<class Node>
    const Node* removeAt(ProtoContext* context, const Node* self, typename Node::KeyType key);

    template<class Node>
    const Node* setAt(ProtoContext* context, const Node* self, typename Node::KeyType key, const ProtoObject* newValue) {
        if (newValue == nullptr) return sparse_avl::removeAt(context, self, key);
        if (self->isEmpty) return new(context) Node(context, key, newValue, nullptr, nullptr, false);

        const uintptr_t w = sparse_avl::keyWord(key);
        const uintptr_t sk = sparse_avl::keyWord(self->key);
        const Node* newNode;
        if (w < sk) {
            const Node* newPrev = self->previous
                ? sparse_avl::setAt(context, self->previous, key, newValue)
                : new(context) Node(context, key, newValue, nullptr, nullptr, false);
            newNode = new(context) Node(context, self->key, self->value, newPrev, self->next, false);
        } else if (w > sk) {
            const Node* newNext = self->next
                ? sparse_avl::setAt(context, self->next, key, newValue)
                : new(context) Node(context, key, newValue, nullptr, nullptr, false);
            newNode = new(context) Node(context, self->key, self->value, self->previous, newNext, false);
        } else {
            if (self->value == newValue) return self;
            newNode = new(context) Node(context, self->key, newValue, self->previous, self->next, false);
        }
        return sparse_avl::rebalance(context, newNode);
    }

    template<class Node>
    const Node* findMin(const Node* node) {
        while (node && node->previous && !node->previous->isEmpty) node = node->previous;
        return node;
    }

    template<class Node>
    const Node* removeAt(ProtoContext* context, const Node* self, typename Node::KeyType key) {
        if (self->isEmpty) return self;

        const uintptr_t w = sparse_avl::keyWord(key);
        const uintptr_t sk = sparse_avl::keyWord(self->key);
        const Node* newNode;
        if (w < sk) {
            if (!self->previous) return self;
            const Node* newPrev = sparse_avl::removeAt(context, self->previous, key);
            if (newPrev == self->previous) return self;
            newNode = new(context) Node(context, self->key, self->value, newPrev, self->next, false);
        } else if (w > sk) {
            if (!self->next) return self;
            const Node* newNext = sparse_avl::removeAt(context, self->next, key);
            if (newNext == self->next) return self;
            newNode = new(context) Node(context, self->key, self->value, self->previous, newNext, false);
        } else {
            if (!self->previous || self->previous->isEmpty) {
                if (!self->next || self->next->isEmpty) return sparse_avl::makeEmpty<Node>(context);
                return self->next;
            }
            if (!self->next || self->next->isEmpty) return self->previous;
            const Node* successor = sparse_avl::findMin(self->next);
            const Node* newNext = sparse_avl::removeAt(context, self->next, successor->key);
            newNode = new(context) Node(context, successor->key, successor->value, self->previous, newNext, false);
        }
        return sparse_avl::rebalance(context, newNode);
    }

    // In-order walk; allocates nothing, so it needs no critical section.
    template<class Node, class Fn>
    void inorder(const Node* node, Fn& fn) {
        while (node && !node->isEmpty) {
            sparse_avl::inorder(node->previous, fn);
            fn(node->key, node->value);
            node = node->next;
        }
    }

    template<class Node, class Iter>
    const Iter* iteratorWithQueue(ProtoContext* context, const Node* self, const Iter* queue) {
        if (self->isEmpty) return queue;
        const Node* node = self;
        const Iter* stack = queue;
        while (node && !node->isEmpty) {
            stack = new(context) Iter(context, ITERATOR_NEXT_THIS, node, stack);
            node = node->previous;
        }
        return stack;
    }

    //--- Small inline form.  keyWord(keys[i]) == 0 marks an empty slot. ---

    template<class Small>
    unsigned long smallCount(const Small* s) {
        unsigned long c = 0;
        for (unsigned i = 0; i < Small::MAX_INLINE; ++i)
            if (sparse_avl::keyWord(s->keys[i]) != 0) ++c;
        return c;
    }

    template<class Small>
    bool smallHas(const Small* s, typename Small::KeyType key) {
        const uintptr_t w = sparse_avl::keyWord(key);
        if (w == 0) return false;
        for (unsigned i = 0; i < Small::MAX_INLINE; ++i)
            if (sparse_avl::keyWord(s->keys[i]) == w) return true;
        return false;
    }

    template<class Small>
    const ProtoObject* smallGetAt(const Small* s, typename Small::KeyType key) {
        const uintptr_t w = sparse_avl::keyWord(key);
        if (w == 0) return nullptr;
        for (unsigned i = 0; i < Small::MAX_INLINE; ++i)
            if (sparse_avl::keyWord(s->keys[i]) == w) return s->values[i];
        return nullptr;
    }

    template<class Small>
    void sortPairs(typename Small::KeyType* ks, const ProtoObject** vs, unsigned n) {
        for (unsigned i = 1; i < n; ++i) {
            const auto ki = ks[i];
            const ProtoObject* vi = vs[i];
            unsigned j = i;
            while (j > 0 && sparse_avl::keyWord(ks[j - 1]) > sparse_avl::keyWord(ki)) {
                ks[j] = ks[j - 1];
                vs[j] = vs[j - 1];
                --j;
            }
            ks[j] = ki;
            vs[j] = vi;
        }
    }

    template<class Small>
    bool smallPairAt(const Small* s, unsigned i, typename Small::KeyType* outKey, const ProtoObject** outValue) {
        if (i >= Small::MAX_INLINE) return false;
        typename Small::KeyType ks[Small::MAX_INLINE];
        const ProtoObject* vs[Small::MAX_INLINE];
        unsigned n = 0;
        for (unsigned j = 0; j < Small::MAX_INLINE; ++j) {
            if (sparse_avl::keyWord(s->keys[j]) != 0) {
                ks[n] = s->keys[j];
                vs[n] = s->values[j];
                ++n;
            }
        }
        sparse_avl::sortPairs<Small>(ks, vs, n);
        if (i >= n) return false;
        if (outKey) *outKey = ks[i];
        if (outValue) *outValue = vs[i];
        return true;
    }

    template<class Small, class Node>
    const Node* smallPromote(ProtoContext* context, const Small* s) {
        const unsigned long n = sparse_avl::smallCount(s);
        const Node* avl = sparse_avl::makeEmpty<Node>(context);
        for (unsigned i = 0; i < n; ++i) {
            typename Small::KeyType k;
            const ProtoObject* v;
            if (sparse_avl::smallPairAt(s, i, &k, &v)) avl = sparse_avl::setAt(context, avl, k, v);
        }
        return avl;
    }

    // setAt on a Small (value == nullptr means removeAt).  Returns the tagged
    // handle of a fresh Small, or of an AVL when the result exceeds
    // MAX_INLINE or the key word is 0 (the empty-slot sentinel).
    template<class Small, class Node>
    const ProtoObject* smallSetAt(ProtoContext* context, const Small* small,
                                  typename Small::KeyType key, const ProtoObject* value) {
        using Key = typename Small::KeyType;
        constexpr unsigned M = Small::MAX_INLINE;
        const uintptr_t w = sparse_avl::keyWord(key);

        if (w == 0 && value != nullptr) {
            const Node* avl = sparse_avl::smallPromote<Small, Node>(context, small);
            return sparse_avl::setAt(context, avl, key, value)->implAsObject(context);
        }

        if (value == nullptr) {
            Key ks[M];
            const ProtoObject* vs[M];
            unsigned n = 0;
            for (unsigned i = 0; i < M; ++i) {
                const uintptr_t kw = sparse_avl::keyWord(small->keys[i]);
                if (kw != 0 && kw != w) {
                    ks[n] = small->keys[i];
                    vs[n] = small->values[i];
                    ++n;
                }
            }
            sparse_avl::sortPairs<Small>(ks, vs, n);
            return (new(context) Small(context, n, ks, vs))->implAsObject(context);
        }

        Key ks[M + 1];
        const ProtoObject* vs[M + 1];
        unsigned n = 0;
        bool replaced = false;
        for (unsigned i = 0; i < M; ++i) {
            const uintptr_t kw = sparse_avl::keyWord(small->keys[i]);
            if (kw == 0) continue;
            if (kw == w) {
                ks[n] = key;
                vs[n] = value;
                replaced = true;
            } else {
                ks[n] = small->keys[i];
                vs[n] = small->values[i];
            }
            ++n;
        }
        if (!replaced) {
            ks[n] = key;
            vs[n] = value;
            ++n;
        }
        sparse_avl::sortPairs<Small>(ks, vs, n);
        if (n <= M) return (new(context) Small(context, n, ks, vs))->implAsObject(context);

        const Node* avl = sparse_avl::makeEmpty<Node>(context);
        for (unsigned i = 0; i < n; ++i) avl = sparse_avl::setAt(context, avl, ks[i], vs[i]);
        return avl->implAsObject(context);
    }

}  // anonymous namespace
}  // namespace proto::sparse_avl

#endif  // PROTOCORE_SPARSE_LIST_ALGORITHMS_H
