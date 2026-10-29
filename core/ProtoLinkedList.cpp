/*
 * ProtoLinkedList.cpp
 *
 * Immutable, collectable singly linked list of cells of four elements,
 * built at the front.  Every cell is full except the head (the list is
 * aligned at its end); every operation preserves that form.
 * Specification: docs/specs/2026-10-25-linked-list-design.md.
 */

#include "../headers/proto_internal.h"

#include <algorithm>
#include <vector>

namespace proto
{
    namespace
    {
        using Node = ProtoLinkedListNode;
        constexpr unsigned SLOTS = Node::SLOTS;

        inline const Node* nodeOf(const ProtoLinkedList* list) {
            return toImpl<const Node>(list);
        }

        // The node of a list, or nullptr for the empty list.
        inline const Node* firstNode(const ProtoLinkedList* list) {
            const Node* node = nodeOf(list);
            return node && node->nodeCount() ? node : nullptr;
        }

        // The empty list: one perennial cell per process, allocated outside
        // the collector (a null context), as symbols are.  It holds no
        // reference, so it needs no root; a function-local static keeps it
        // reachable for the process.
        const Node* emptyNode() {
            static const Node* const empty =
                new(static_cast<ProtoContext*>(nullptr)) Node(nullptr, 0, nullptr, nullptr);
            return empty;
        }

        inline const ProtoLinkedList* emptyList(ProtoContext*) {
            return emptyNode()->asLinkedList();
        }

        inline const ProtoLinkedList* listOf(ProtoContext* context, const Node* node) {
            return node ? node->asLinkedList() : emptyList(context);
        }

        // `values[0..count)` (no nullptr) followed by `rest`, whose head is
        // full or which is nullptr.  The cells are built from the end, so the
        // only partial cell is the head: ((count - 1) mod 4) + 1 elements.
        // `values` must stay reachable from the caller's roots.
        const Node* buildFront(ProtoContext* context, const ProtoObject* const* values,
                               proto_ulong count, const Node* rest) {
            if (count == 0) return rest;
            // Heap-ceiling backpressure before the first allocation, with
            // nothing half-built (as ProtoContext::newList does).
            context->heapLimitCheckpoint();
            const unsigned head = static_cast<unsigned>((count - 1) % SLOTS) + 1;
            const Node* node = rest;
            if (count == head) {
                return new(context) Node(context, head, values, node);
            }
            // A long build stays open to stop-the-world.  The chain built so
            // far needs no anchor: every cell of it was allocated through
            // `context`, so it is on that context's young generation, which
            // the stop-the-world root scan records as a root, and nothing
            // here submits that generation (only ProtoContext::safepoint()
            // and ~ProtoContext do; parkForStopTheWorld parks without
            // submitting).  See ProtoContext::newList(n, items).
            for (proto_ulong end = count; end > head; end -= SLOTS) {
                node = new(context) Node(context, SLOTS, values + (end - SLOTS), node);
                parkForStopTheWorld(context);
            }
            return new(context) Node(context, head, values, node);
        }

        // The elements of a list from `node` on, appended to `out`.
        void gather(const Node* node, std::vector<const ProtoObject*>& out) {
            for (; node; node = node->next) {
                for (unsigned i = 0; i < node->nodeCount(); ++i) out.push_back(node->slot[i]);
            }
        }

        // `values` (no nullptr) followed by the list `tail`.  A partial head
        // of `tail` is copied, merged after `values`; its full cells are
        // shared.
        const ProtoLinkedList* buildBefore(ProtoContext* context,
                                           std::vector<const ProtoObject*>& values,
                                           const Node* tail) {
            if (tail && tail->nodeCount() < SLOTS) {
                for (unsigned i = 0; i < tail->nodeCount(); ++i) values.push_back(tail->slot[i]);
                tail = tail->next;
            }
            return listOf(context, buildFront(context, values.data(), values.size(), tail));
        }

        // Copies `values`, skipping nullptr entries, only when there are any.
        const ProtoLinkedList* buildFromArray(ProtoContext* context, proto_ulong count,
                                              const ProtoObject* const* values) {
            proto_ulong nulls = 0;
            for (proto_ulong i = 0; i < count; ++i) nulls += values[i] == nullptr;
            if (nulls == 0) return listOf(context, buildFront(context, values, count, nullptr));
            std::vector<const ProtoObject*> kept;
            kept.reserve(count - nulls);
            for (proto_ulong i = 0; i < count; ++i) if (values[i]) kept.push_back(values[i]);
            return listOf(context, buildFront(context, kept.data(), kept.size(), nullptr));
        }
    }

    //=========================================================================
    // ProtoLinkedListNode
    //=========================================================================

    ProtoLinkedListNode::ProtoLinkedListNode(ProtoContext* context, unsigned n,
                                             const ProtoObject* const* values,
                                             const ProtoLinkedListNode* rest)
        : Cell(context),
          counts(static_cast<proto_ulong>(n)
                 | ((n + (rest ? rest->totalCount() : 0)) << NODE_COUNT_BITS)),
          next(rest),
          slot{}
    {
        for (unsigned i = 0; i < n; ++i) slot[i] = values[i];
    }

    const ProtoLinkedList* ProtoLinkedListNode::asLinkedList() const {
        ProtoObjectPointer p{};
        p.linkedListNode = this;
        p.op.pointer_tag = POINTER_TAG_LINKED_LIST;
        return p.linkedList;
    }

    const ProtoObject* ProtoLinkedListNode::implAsObject(ProtoContext*) const {
        return reinterpret_cast<const ProtoObject*>(asLinkedList());
    }

    void ProtoLinkedListNode::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (next) method(context, self, next);
        for (unsigned i = 0; i < nodeCount(); ++i) {
            if (const Cell* c = ProtoObject::asCellPointer(slot[i])) method(context, self, c);
        }
    }

    proto_ulong ProtoLinkedListNode::getHash(ProtoContext* context) const {
        // ProtoTupleImplementation::getHash over the same elements, so that a
        // list and its asTuple hash alike.
        proto_ulong h = PROTO_UL(0x345678) ^ (totalCount() * PROTO_UL(0xa6b3f7) + PROTO_UL(1));
        for (const Node* node = this; node; node = node->next) {
            for (unsigned i = 0; i < node->nodeCount(); ++i) {
                h = (h * PROTO_UL(1000003)) ^ node->slot[i]->getHash(context);
            }
        }
        return h;
    }

    //=========================================================================
    // ProtoLinkedList
    //=========================================================================

    const ProtoObject* ProtoLinkedList::getFirst(ProtoContext*) const {
        const Node* node = firstNode(this);
        return node ? node->slot[0] : nullptr;
    }

    const ProtoLinkedList* ProtoLinkedList::getRest(ProtoContext* context) const {
        const Node* node = firstNode(this);
        if (!node) return this;
        if (node->nodeCount() == 1) return listOf(context, node->next);
        return (new(context) Node(context, node->nodeCount() - 1, node->slot + 1, node->next))
            ->asLinkedList();
    }

    proto_ulong ProtoLinkedList::getSize(ProtoContext*) const {
        const Node* node = nodeOf(this);
        return node ? node->totalCount() : 0;
    }

    bool ProtoLinkedList::isEmpty(ProtoContext* context) const {
        return getSize(context) == 0;
    }

    const ProtoLinkedList* ProtoLinkedList::prepend(ProtoContext* context, const ProtoObject* value) const {
        if (!value) return this;
        const Node* node = firstNode(this);
        if (node && node->nodeCount() < SLOTS) {
            const ProtoObject* values[SLOTS];
            values[0] = value;
            for (unsigned i = 0; i < node->nodeCount(); ++i) values[i + 1] = node->slot[i];
            return (new(context) Node(context, node->nodeCount() + 1, values, node->next))
                ->asLinkedList();
        }
        return (new(context) Node(context, 1, &value, node))->asLinkedList();
    }

    const ProtoObject* ProtoLinkedList::getAt(ProtoContext*, proto_ulong index) const {
        const Node* node = firstNode(this);
        if (!node || index >= node->totalCount()) return nullptr;
        while (index >= node->nodeCount()) {
            index -= node->nodeCount();
            node = node->next;
        }
        return node->slot[index];
    }

    const ProtoLinkedList* ProtoLinkedList::removeFirst(ProtoContext* context, proto_ulong count) const {
        const Node* node = firstNode(this);
        if (!node || count == 0) return this;
        if (count >= node->totalCount()) return emptyList(context);
        while (count >= node->nodeCount()) {
            count -= node->nodeCount();
            node = node->next;
        }
        if (count == 0) return node->asLinkedList();
        // A suffix of one cell followed by full cells is canonical.
        return (new(context) Node(context, node->nodeCount() - static_cast<unsigned>(count),
                                  node->slot + count, node->next))->asLinkedList();
    }

    const ProtoLinkedList* ProtoLinkedList::takeFirst(ProtoContext* context, proto_ulong count) const {
        const Node* node = firstNode(this);
        if (!node || count >= node->totalCount()) return this;
        if (count == 0) return emptyList(context);
        std::vector<const ProtoObject*> values;
        values.reserve(count);
        for (; values.size() < count; node = node->next) {
            for (unsigned i = 0; i < node->nodeCount() && values.size() < count; ++i)
                values.push_back(node->slot[i]);
        }
        return listOf(context, buildFront(context, values.data(), values.size(), nullptr));
    }

    const ProtoLinkedList* ProtoLinkedList::prependAll(ProtoContext* context, const ProtoLinkedList* tail) const {
        const Node* node = firstNode(this);
        const Node* tailNode = firstNode(tail);
        if (!node) return tail ? tail : this;
        if (!tailNode) return this;
        std::vector<const ProtoObject*> values;
        values.reserve(node->totalCount() + SLOTS);
        gather(node, values);
        return buildBefore(context, values, tailNode);
    }

    const ProtoLinkedList* ProtoLinkedList::reverse(ProtoContext* context) const {
        const Node* node = firstNode(this);
        if (!node || node->totalCount() == 1) return this;
        std::vector<const ProtoObject*> values;
        values.reserve(node->totalCount());
        gather(node, values);
        std::reverse(values.begin(), values.end());
        return listOf(context, buildFront(context, values.data(), values.size(), nullptr));
    }

    bool ProtoLinkedList::isEqual(ProtoContext*, const ProtoLinkedList* other) const {
        if (!other) return false;
        const Node* a = firstNode(this);
        const Node* b = firstNode(other);
        if (!a || !b) return a == b;
        if (a->totalCount() != b->totalCount()) return false;
        // Equal sizes give equal cell boundaries (canonical form), so the
        // walk compares cell by cell and stops at a shared suffix.
        for (; a && a != b; a = a->next, b = b->next) {
            for (unsigned i = 0; i < a->nodeCount(); ++i) {
                if (a->slot[i] != b->slot[i]) return false;
            }
        }
        return true;
    }

    proto_ulong ProtoLinkedList::getHash(ProtoContext* context) const {
        return nodeOf(this)->getHash(context);
    }

    const ProtoList* ProtoLinkedList::asList(ProtoContext* context) const {
        std::vector<const ProtoObject*> values;
        values.reserve(getSize(context));
        gather(firstNode(this), values);
        return context->newList(static_cast<unsigned>(values.size()), values.data());
    }

    const ProtoTuple* ProtoLinkedList::asTuple(ProtoContext* context) const {
        std::vector<const ProtoObject*> values;
        values.reserve(getSize(context));
        gather(firstNode(this), values);
        return context->newTuple(values);
    }

    const ProtoObject* ProtoLinkedList::asObject(ProtoContext*) const {
        return reinterpret_cast<const ProtoObject*>(this);
    }

    //=========================================================================
    // ProtoLinkedListCursor
    //=========================================================================

    ProtoLinkedListCursor::ProtoLinkedListCursor(const ProtoLinkedList* list)
        : node(firstNode(list)), index(0) {}

    bool ProtoLinkedListCursor::hasNext() const {
        return node != nullptr;
    }

    const ProtoObject* ProtoLinkedListCursor::next() {
        const Node* current = static_cast<const Node*>(node);
        if (!current) return nullptr;
        const ProtoObject* value = current->slot[index];
        if (++index == current->nodeCount()) {
            node = current->next;
            index = 0;
        }
        return value;
    }

    //=========================================================================
    // ProtoObject type test and coercion
    //=========================================================================

    bool ProtoObject::isLinkedList(ProtoContext* context) const {
        if (!this) return false;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_LINKED_LIST) return true;
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->isLinkedList(context);
        }
        return false;
    }

    const ProtoLinkedList* ProtoObject::asLinkedList(ProtoContext* context) const {
        if (!this) return nullptr;
        ProtoObjectPointer pa{}; pa.oid = this;
        if (pa.op.pointer_tag == POINTER_TAG_LINKED_LIST)
            return reinterpret_cast<const ProtoLinkedList*>(this);
        if (pa.op.pointer_tag == POINTER_TAG_OBJECT) {
            const proto::ProtoString* dataName = context->space->literalData;
            const proto::ProtoObject* data = this->getAttribute(context, dataName, false);
            if (data && data != this) return data->asLinkedList(context);
        }
        return nullptr;
    }

    //=========================================================================
    // ProtoContext builders and ProtoSpace accessors
    //=========================================================================

    const ProtoLinkedList* ProtoContext::newLinkedList() {
        return emptyList(this);
    }

    const ProtoLinkedList* ProtoContext::newLinkedListFromArray(proto_ulong count, const ProtoObject* const* values) {
        if (count == 0 || !values) return emptyList(this);
        return buildFromArray(this, count, values);
    }

    const ProtoLinkedList* ProtoContext::newLinkedListFromList(const ProtoList* list) {
        const proto_ulong size = list ? list->getSize(this) : 0;
        std::vector<const ProtoObject*> values;
        values.reserve(size);
        for (proto_ulong i = 0; i < size; ++i) values.push_back(list->getAt(this, static_cast<int>(i)));
        return buildFromArray(this, values.size(), values.data());
    }

    const ProtoLinkedList* ProtoContext::newLinkedListFromTuple(const ProtoTuple* tuple) {
        const proto_ulong size = tuple ? tuple->getSize(this) : 0;
        std::vector<const ProtoObject*> values;
        values.reserve(size);
        for (proto_ulong i = 0; i < size; ++i) values.push_back(tuple->getAt(this, static_cast<int>(i)));
        return buildFromArray(this, values.size(), values.data());
    }

    ProtoObject* ProtoSpace::getLinkedListPrototype() const {
        return impl->linkedListPrototype;
    }

    void ProtoSpace::setLinkedListPrototype(ProtoObject* prototype) {
        auto* extension = const_cast<ProtoSpaceImplementation*>(impl);
        const ProtoRootSet::Handle previous = extension->linkedListPrototypeHandle;
        extension->linkedListPrototypeHandle = extension->roots->add(prototype);
        extension->linkedListPrototype = prototype;
        extension->roots->remove(previous);
    }
}
