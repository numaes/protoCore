/*
 * ProtoLazyObject.cpp
 *
 * Lazy objects: immutable object cells with a value computed at most once,
 * on demand, by a producer (ProtoObject::newLazyChild, headers/protoCore.h).
 *
 * The collector-safety argument is in the comment on ProtoLazyObjectCell
 * (headers/proto_internal.h). In short: the producer is written once, in the
 * constructor; the value is written once, by the caller whose
 * compare-and-set moved the state to Publishing, before the release store
 * of Realized; processReferences reads the state before the value; and the
 * producer stops being traced only from the first cycle whose pause found
 * the value already published (the epoch packed with the state).
 *
 * What this type is for: a value cached in a MUTABLE object lives in the
 * global mutable table, whose states are collector roots until the object's
 * handle is swept. A chain of such objects, each state pointing at the next
 * object, is therefore freed one link per cycle: a lazy sequence walked
 * without holding its head stays alive. A lazy object's value is an
 * ordinary field of a cell, traced only when the cell is, so an unreachable
 * chain of lazy objects is freed in the cycle that finds it unreachable.
 */

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

namespace proto
{
    namespace {
        constexpr uint64_t kStateMask = 3;

        inline int stateOf(uint64_t word) { return static_cast<int>(word & kStateMask); }

        // The lazy cell behind `o`, or nullptr when `o` is not a lazy object.
        inline const ProtoLazyObjectCell* lazyCellOf(const ProtoObject* o) {
            if (!o) return nullptr;
            ProtoObjectPointer pa{};
            pa.oid = o;
            if (pa.op.pointer_tag != POINTER_TAG_OBJECT) return nullptr;
            const auto* cell = reinterpret_cast<const Cell*>(pa.voidPointer);
            if (cell->getType() != CellType::Object) return nullptr;
            return static_cast<const ProtoObjectCell*>(cell)->asLazyCell();
        }
    }

    ProtoLazyObjectCell::ProtoLazyObjectCell(ProtoContext* context,
                                             const ParentLinkImplementation* parent,
                                             const ProtoSparseListImplementation* attributes,
                                             const ProtoObject* producer)
        : ProtoObjectCell(context, parent, attributes, 0),
          producer(producer ? producer : PROTO_NONE),
          value(nullptr),
          stateEpoch(ProtoObject::LazyUnrealized) {}

    void ProtoLazyObjectCell::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        ProtoObjectCell::processReferences(context, self, method);
        // ORDER IS LOAD-BEARING: the state before the value (see the class
        // comment). A Realized state guarantees the value store is visible.
        const uint64_t word = stateEpoch.load(std::memory_order_acquire);
        const int state = stateOf(word);
        if (state == ProtoObject::LazyRealized || state == ProtoObject::LazyPublishing) {
            if (const Cell* v = ProtoObject::asCellPointer(value.load(std::memory_order_acquire)))
                method(context, self, v);
        }
        // The producer is released from the first cycle whose pause found the
        // value published: its epoch is then older than the current one.
        const bool released = state == ProtoObject::LazyRealized &&
            (word >> 2) < multispace::gcEpoch.load(std::memory_order_acquire);
        if (!released) {
            if (const Cell* p = ProtoObject::asCellPointer(producer)) method(context, self, p);
        }
    }

    bool ProtoObject::isLazyObject(ProtoContext*) const {
        return lazyCellOf(this) != nullptr;
    }

    int ProtoObject::getLazyState(ProtoContext*) const {
        const ProtoLazyObjectCell* c = lazyCellOf(this);
        if (!c) return LazyNotLazy;
        return stateOf(c->stateEpoch.load(std::memory_order_acquire));
    }

    const ProtoObject* ProtoObject::getLazyProducer(ProtoContext*) const {
        const ProtoLazyObjectCell* c = lazyCellOf(this);
        if (!c) return nullptr;
        if (stateOf(c->stateEpoch.load(std::memory_order_acquire)) == LazyRealized) return nullptr;
        return c->producer;
    }

    const ProtoObject* ProtoObject::getLazyValue(ProtoContext*) const {
        const ProtoLazyObjectCell* c = lazyCellOf(this);
        if (!c) return nullptr;
        if (stateOf(c->stateEpoch.load(std::memory_order_acquire)) != LazyRealized) return nullptr;
        return c->value.load(std::memory_order_acquire);
    }

    bool ProtoObject::claimLazy(ProtoContext*) const {
        const ProtoLazyObjectCell* c = lazyCellOf(this);
        if (!c) return false;
        uint64_t expected = c->stateEpoch.load(std::memory_order_acquire);
        if (stateOf(expected) != LazyUnrealized) return false;
        return c->stateEpoch.compare_exchange_strong(
            expected, (expected & ~kStateMask) | LazyClaimed,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void ProtoObject::unclaimLazy(ProtoContext*) const {
        const ProtoLazyObjectCell* c = lazyCellOf(this);
        if (!c) return;
        uint64_t expected = c->stateEpoch.load(std::memory_order_acquire);
        if (stateOf(expected) != LazyClaimed) return;
        c->stateEpoch.compare_exchange_strong(
            expected, (expected & ~kStateMask) | LazyUnrealized,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool ProtoObject::publishLazy(ProtoContext*, const ProtoObject* v) const {
        const ProtoLazyObjectCell* c = lazyCellOf(this);
        if (!c) return false;
        uint64_t expected = c->stateEpoch.load(std::memory_order_acquire);
        for (;;) {
            const int state = stateOf(expected);
            if (state == LazyRealized || state == LazyPublishing) return false;
            if (c->stateEpoch.compare_exchange_weak(
                    expected, (expected & ~kStateMask) | LazyPublishing,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                break;
        }
        // This caller alone writes the value. No allocation and no protoCore
        // call from here to the Realized store, so no pause can fall between
        // them: the epoch read here is the one of the cycle the store lands in.
        c->value.store(v ? v : PROTO_NONE, std::memory_order_release);
        const uint64_t epoch = multispace::gcEpoch.load(std::memory_order_acquire);
        c->stateEpoch.store((epoch << 2) | LazyRealized, std::memory_order_release);
        return true;
    }
}  // namespace proto
