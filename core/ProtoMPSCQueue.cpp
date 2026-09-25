/*
 * ProtoMPSCQueue.cpp
 *
 * A mutable, lock-free, multi-producer / single-consumer FIFO of
 * `ProtoObject*` items whose contents are traced by the collector.
 * Specification: protoScala/docs/platform/PMQ-SPEC.md.
 *
 * ---------------------------------------------------------------------
 * Why this is safe under protoCore's concurrent mark, with no barrier
 * and no stop-the-world work
 * ---------------------------------------------------------------------
 *
 * protoCore stops the world only to take thread stack roots, the
 * mutables-tree snapshot and the roots of global structures; the mark
 * phase then runs concurrently with the mutators (core/ProtoSpace.cpp,
 * the world resumes before the mark loop).  That is sound for every
 * other type because everything reachable from those roots is immutable:
 * the marker walks a frozen graph.
 *
 * This queue is mutable, so it has to earn the same property.  It does
 * not do it by being captured under the pause; it does it by making a
 * lazy read of its two mutable words a SUPERSET of what the pause saw.
 *
 * Definitions.  Let S be the instant the world resumed for the running
 * cycle, and H_S the node chain hanging from `head` at S.  Nodes are
 * immutable once published: `item` is written in the constructor and
 * `next` only before the publishing CAS succeeds.
 *
 *   push     allocates a node, sets node->next to the current head and
 *            CASes head from that value to the node.  It only ever
 *            PREPENDS, and it publishes a fully linked chain: at every
 *            instant, walking from `head` reaches every node in the
 *            queue.  (This is why a Vyukov exchange-based MPSC queue
 *            cannot be used: it leaves the chain briefly unlinked, and
 *            while a consumer may spin for the missing link, the
 *            collector may not.)
 *
 *   takeAll  publishes a retain cell holding the chain onto `retained`
 *            BEFORE detaching the chain from `head`, and releases the
 *            previous content of `retained` only when the GC cycle
 *            counter has moved on.
 *
 * Claim.  If the marker processes this cell at time T > S, loading
 * `head` at T1 and `retained` at T2 > T1, then every node of H_S is
 * reported.
 *
 * Proof.  Take n in H_S.  Either n is still in the chain at `head` at
 * T1 - reported - or some takeAll removed it before T1.  Consider the
 * first takeAll that removed n.  It loaded h from `head` at a time
 * after S, and nodes are only prepended, so chain(h) contains every
 * node of H_S not already removed; by induction on the sequence of
 * takeAlls, n is in chain(h).  That takeAll published a retain cell
 * carrying h onto `retained` before the detach, and `retained` only
 * grows during a cycle (its release is gated on the cycle counter
 * changing, and the counter is bumped under the pause at the START of
 * a cycle, core/ProtoSpace.cpp).  So n is reachable from `retained` at
 * every instant after its removal, in particular at T2.  QED.
 *
 * The two orderings are therefore load-bearing, not stylistic:
 *   * processReferences MUST load `head` before `retained`;
 *   * takeAll MUST publish the retain cell before it detaches.
 * Swap either and an item pushed before the pause and consumed during
 * the mark is freed under a live ProtoList.
 *
 * ---------------------------------------------------------------------
 * The nodes prepended INSIDE the publish window, and why the retain cell
 * is widened after the detach
 * ---------------------------------------------------------------------
 *
 * A producer may prepend at any instant between this consumer's `head`
 * load (h) and its detaching exchange.  The chain the exchange returns
 * is therefore a SUPERSET of chain(h): call the extra nodes W.  Until
 * 2.3.1 the retain cell published only h, and the argument for W was
 * that its nodes "were allocated after S, so they are young cells of the
 * pushing context and are not candidates of the running cycle".
 *
 * That argument is true and insufficient.  It covers only the cycle that
 * was running when the window closed.  `takeAll` then walks the detached
 * chain and parks for stop-the-world every kPollInterval nodes (below),
 * so the call routinely spans a cycle BOUNDARY.  For the next cycle the
 * nodes of W are ordinary candidates - the pushing context submitted its
 * young chain when it ended its turn - and between the detach and the
 * end of the walk they hang off nothing but a C++ local.  The collector
 * has no view of C++ locals, so it frees them and the items they are the
 * only reference to: a batch of 182 lost its LAST element, which is
 * exactly the one node a producer had prepended inside the window (the
 * chain is LIFO, so the newest node is the last item out).
 *
 * The fix is one store: after the exchange, publish the chain that was
 * actually detached into the same retain cell.
 *
 *   * It cannot narrow anything.  Only prepends happen, so chain(h) is a
 *     suffix of chain(detached); the store replaces a set by a superset
 *     of itself and the proof above still reads `retained` for h.
 *   * The ordering the proof needs is untouched: the retain cell is
 *     still PUBLISHED onto `retained` before the detach.  Only its
 *     contents are widened afterwards, and a marker that loads either
 *     value is correct.
 *   * It is inside the window, hence before the first park.  A pause can
 *     only complete once this thread parks, and it cannot park at
 *     criticalSectionDepth > 0; so every cycle for which W is a
 *     candidate set starts strictly after the widening store is visible.
 *   * A marker that already processed this retain cell in the CURRENT
 *     cycle will not revisit it (marked cells are never re-pushed), so
 *     the widening may be invisible to that cycle - which is precisely
 *     the cycle for which the original young-cell argument holds.  The
 *     two arguments are complementary and together cover every cycle.
 *   * The retain cell itself outlives the walk: `retained` is released
 *     only by a later takeAll on the same single consumer thread, and
 *     that thread is inside this one.
 *
 * So after the widening every node of the detached chain, and every item
 * hanging off it, is reachable from a GC root at every instant from the
 * detach to the return - which is what the O(batch) walk below needs in
 * order to be allowed to park at all.
 *
 * The publish window is a ProtoContext::CriticalSection containing no
 * allocation and no safepoint, so no pause can complete inside it and
 * that window cannot span a cycle boundary.
 *
 * The same guard proves ABA impossible on the push CAS: reusing the
 * address of the node `head` was loaded from would require a sweep to
 * complete between the load and the CAS, a sweep requires a pause, and
 * a pause requires this thread to park - which it does only in
 * allocCell / safepoint, neither of which is reached inside the window.
 * NEVER add an allocation or a protoCore call inside a publish window.
 *
 * Release of `retained` is done by the consumer, at its first takeAll
 * in a new GC cycle.  Observing counter C proves the pause of cycle C
 * happened, which proves the mark and sweep of cycle C-1 finished (one
 * collector thread, STW then mark then sweep in sequence), which proves
 * everything being released was already traced.  It is NOT a write
 * barrier: no work proportional to the references involved, nothing the
 * collector reads, and omitting it could only retain memory - never
 * lose an object.  It is also not done by processReferences: the young-
 * chain walk calls processReferences too (core/ProtoSpace.cpp Phase 4),
 * so a destructive read there would be a second, silent consumer.
 */

#include "../headers/proto_internal.h"

#include <algorithm>
#include <vector>

namespace proto
{
    //=========================================================================
    // Test-only intervention point
    //=========================================================================
    // The bug this hook exists to reproduce lives in a window that a producer
    // can only enter by racing, and a flaky reproduction is not a test: the
    // strongest statistical case the race gave was 4 failures in 40 runs
    // against 0 in 40, i.e. p ~ 0.12, which does not distinguish a fix from
    // luck.  The hook makes the interleaving a decision instead of a race, so
    // that `test/ProtoMPSCQueueWindowTests.cpp` fails deterministically when
    // the widening store below is removed.
    //
    // It is null in every build and the library never installs it; the cost on
    // the live path is one relaxed atomic load per takeAll, on a path that
    // already does an exchange and a CAS.  It is declared in proto_internal.h,
    // never in the public header: an embedder cannot reach it.
    std::atomic<PmqTakeAllWindowHook> pmqTakeAllWindowHook{nullptr};

    namespace {
        inline void runWindowHook(ProtoContext* context, const ProtoMPSCQueue* queue,
                                  PmqWindowPhase phase) {
            if (const PmqTakeAllWindowHook hook =
                    pmqTakeAllWindowHook.load(std::memory_order_relaxed))
                hook(context, queue, phase);
        }
    }

    //=========================================================================
    // ProtoMPSCQueueImplementation
    //=========================================================================
    ProtoMPSCQueueImplementation::ProtoMPSCQueueImplementation(ProtoContext* context)
        : Cell(context), head(nullptr), retained(nullptr), retainedEpoch(0) {}

    const ProtoObject* ProtoMPSCQueueImplementation::implAsObject(ProtoContext*) const {
        ProtoObjectPointer p{};
        p.mpscQueueImplementation = this;
        p.op.pointer_tag = POINTER_TAG_MPSC_QUEUE;
        return p.oid;
    }

    void ProtoMPSCQueueImplementation::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        // ORDER IS LOAD-BEARING.  `head` first, `retained` second.  A takeAll
        // publishes into `retained` before it removes a chain from `head`, so
        // reading in this order makes the union a superset of the chain that
        // existed when the world resumed.  Reading them the other way round
        // loses every item consumed between the two loads.  See the proof at
        // the top of this file.
        if (const ProtoMPSCQueueNodeImplementation* h = head.load(std::memory_order_acquire))
            method(context, self, h);
        if (const ProtoMPSCQueueRetainImplementation* r = retained.load(std::memory_order_acquire))
            method(context, self, r);
    }

    //=========================================================================
    // ProtoMPSCQueueNodeImplementation
    //=========================================================================
    ProtoMPSCQueueNodeImplementation::ProtoMPSCQueueNodeImplementation(
        ProtoContext* context, const ProtoObject* i)
        : Cell(context), item(i), next(nullptr) {}

    const ProtoObject* ProtoMPSCQueueNodeImplementation::implAsObject(ProtoContext*) const {
        // Internal cell (PMQ-SPEC section 4, decision D6): the raw, untagged
        // address.  It is never handed to newChild / addParent /
        // getAttribute, so it can never reach the attribute chain.
        return reinterpret_cast<const ProtoObject*>(this);
    }

    void ProtoMPSCQueueNodeImplementation::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        // An item whose only reference is this node must survive.  Embedded
        // items (SmallInteger, boolean, char, None, inline string) carry no
        // cell and are skipped - reporting one would abort the collector in
        // instrumented builds (pushReportedReference).
        if (const Cell* c = ProtoObject::asCellPointer(item)) method(context, self, c);
        // The chain is reported link by link rather than walked here: the
        // mark loop's work list is iterative, so a million-node chain costs
        // a million pushes and no stack depth.
        if (const ProtoMPSCQueueNodeImplementation* n = next.load(std::memory_order_acquire))
            method(context, self, n);
    }

    //=========================================================================
    // ProtoMPSCQueueRetainImplementation
    //=========================================================================
    ProtoMPSCQueueRetainImplementation::ProtoMPSCQueueRetainImplementation(ProtoContext* context)
        : Cell(context), chain(nullptr), next(nullptr) {}

    const ProtoObject* ProtoMPSCQueueRetainImplementation::implAsObject(ProtoContext*) const {
        return reinterpret_cast<const ProtoObject*>(this);
    }

    void ProtoMPSCQueueRetainImplementation::processReferences(
        ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (const ProtoMPSCQueueNodeImplementation* c = chain.load(std::memory_order_acquire))
            method(context, self, c);
        if (const ProtoMPSCQueueRetainImplementation* n = next.load(std::memory_order_acquire))
            method(context, self, n);
    }

    namespace {
        using QueueCell  = ProtoMPSCQueueImplementation;
        using NodeCell   = ProtoMPSCQueueNodeImplementation;
        using RetainCell = ProtoMPSCQueueRetainImplementation;

        inline const QueueCell* implOf(const ProtoMPSCQueue* q) {
            return toImpl<const QueueCell>(q);
        }
    }

    //=========================================================================
    // Public trampolines
    //=========================================================================
    void ProtoMPSCQueue::push(ProtoContext* context, const ProtoObject* item) const
    {
        const QueueCell* q = implOf(this);

        // The cell is allocated INSIDE the critical section so the heap
        // ceiling is enforced at the outermost boundary (CriticalSection's
        // constructor runs heapLimitCheckpoint at depth 0) and so that no
        // stop-the-world can complete between the allocation and the
        // publishing CAS.  That is what makes ABA impossible on `head`:
        // reusing the address this loop loaded would require a sweep, a
        // sweep requires a pause, and a pause requires this thread to park.
        ProtoContext::CriticalSection cs(context);

        NodeCell* node = new(context) NodeCell(context, item);

        // No allocation and no protoCore call from here to the CAS.
        const NodeCell* h = q->head.load(std::memory_order_relaxed);
        do {
            node->next.store(h, std::memory_order_relaxed);
        } while (!q->head.compare_exchange_weak(
                     h, node, std::memory_order_release, std::memory_order_relaxed));
    }

    bool ProtoMPSCQueue::isEmpty(ProtoContext*) const
    {
        // `retained` is deliberately ignored: its chains have already been
        // consumed and are kept only for the collector.
        return implOf(this)->head.load(std::memory_order_acquire) == nullptr;
    }

    const ProtoList* ProtoMPSCQueue::takeAll(ProtoContext* context) const
    {
        const QueueCell* q = implOf(this);

        // Fast path: nothing queued, nothing allocated but the empty list.
        if (q->head.load(std::memory_order_acquire) == nullptr)
            return context->newList();

        const NodeCell* chain = nullptr;
        {
            // The retain cell is allocated before the window: allocating
            // inside it would be an allocation in a region that must contain
            // none (see push).
            RetainCell* retain = new(context) RetainCell(context);

            ProtoContext::CriticalSection cs(context);

            // --- publish window: no allocation, no safepoint, no protoCore
            // --- call.  No stop-the-world can complete inside it, so the
            // --- cycle number read here is still current at the publish.
            const uint64_t cycle = context->space->getGCCycleCount();
            if (q->retainedEpoch.load(std::memory_order_relaxed) != cycle) {
                // Everything currently retained was detached in an earlier
                // cycle, so the mark that had to see it has finished.
                q->retained.exchange(nullptr, std::memory_order_acq_rel);
                q->retainedEpoch.store(cycle, std::memory_order_relaxed);
            }

            const NodeCell* h = q->head.load(std::memory_order_acquire);
            if (h == nullptr) {
                // A racing consumer emptied the queue between the fast-path
                // probe and here (a caller bug, PMQ-SPEC section 2 - but it
                // must stay memory-safe).  The retain cell becomes garbage.
                return context->newList();
            }

            // A test may prepend here, where a producer that ran between the
            // load of `h` and the retain publish would have.
            runWindowHook(context, this, PmqWindowPhase::AfterHeadLoad);

            retain->chain.store(h, std::memory_order_relaxed);
            const RetainCell* rh = q->retained.load(std::memory_order_relaxed);
            do {
                retain->next.store(rh, std::memory_order_relaxed);
            } while (!q->retained.compare_exchange_weak(
                         rh, retain, std::memory_order_release, std::memory_order_relaxed));

            // Only now may the chain leave `head`.  Producers may have
            // prepended since the load above; those nodes are taken too and
            // are not covered by `retain` - they were allocated inside this
            // window, hence after the pause, hence young cells of the pushing
            // context that this cycle's sweep cannot see.
            //
            // A test may prepend here, where a producer that ran between the
            // retain publish and the detach would have.
            runWindowHook(context, this, PmqWindowPhase::BeforeDetach);

            chain = q->head.exchange(nullptr, std::memory_order_acq_rel);

            // Widen the retain cell to the chain that was ACTUALLY detached.
            //
            // This is the fix for the loss described at the top of this file:
            // the young-cell argument for the nodes prepended inside this
            // window covers only the cycle that is running now, and the
            // O(batch) walk below parks, so the call outlives that cycle.  The
            // store cannot narrow anything - only prepends happen, so
            // chain(h) is a suffix of chain(chain) - and it leaves the
            // publish-before-detach ordering the proof depends on untouched,
            // because the cell was already published above.  It is still
            // inside the window, so it is visible before this thread can park,
            // hence before any cycle for which these nodes are candidates can
            // begin.
            //
            // It is a store and not an allocation or a protoCore call, so the
            // window's ABA argument is intact: there is still nothing here
            // that can complete a sweep between the `head` load and the push
            // CAS that races it.
            retain->chain.store(chain, std::memory_order_release);
            // --- end of the publish window ---
        }

        // Outside OUR critical section on purpose: this is O(batch) and a
        // critical section here would delay a stop-the-world for the length
        // of the batch.  The items stay reachable through the retained chain
        // while the list is built.
        //
        // Being outside the section is necessary but was not sufficient.  A
        // loop that makes no protoCore call never reaches a stop-the-world
        // poll either, so until this walk polled explicitly the pause was
        // still linear in the batch: 340 us at 50,000 items rising to 3.67 ms
        // at 400,000, against a flat 27-34 us for a plain bulk build of the
        // same sizes (PMQ-SPEC section 3 constraint 1).  Both O(batch) loops
        // below therefore park every kPollInterval nodes, the same remedy
        // newList(n, items) applies to its own build loop.
        //
        // WHERE THE POLL SITS, AND WHY IT IS NOT IN THE PUBLISH WINDOW.
        // Everything below runs after `chain` has already been detached, i.e.
        // strictly after the window (read epoch -> maybe release -> load head
        // -> fill and publish the retain cell -> detach) has closed and its
        // CriticalSection has been destroyed.  Nothing was moved into that
        // window and nothing inside it can park: the window's last statement
        // is the detaching exchange, and the first poll is reached only once
        // the enclosing scope has ended.  The ABA argument of PMQ-SPEC
        // section 7 is therefore untouched - reusing the address loaded from
        // `head` would still require a sweep to complete between that load
        // and the CAS, which still requires this thread to park between them,
        // which it still cannot do: there is no allocation and no protoCore
        // call in that span, and parkForStopTheWorld is a no-op at
        // criticalSectionDepth > 0 in any case.  NEVER move a poll, an
        // allocation or any protoCore call above the end of that scope.
        //
        // Parking here is safe for the opposite reason to newList's: nothing
        // this loop still needs lives only in a C++ local.  The nodes hang
        // off the retain cell this takeAll published onto `retained`, the
        // items hang off the nodes, and `retained` is only released by a
        // later takeAll on this same single consumer thread - which cannot
        // run while this one is in progress.  A collection that lands on one
        // of these polls traces the whole batch from the queue cell, which
        // the caller holds live for the duration of the call (the same
        // precondition the design already has: `retained` protects nothing if
        // the queue itself is unreachable).
        //
        // Every 64 nodes, matching allocCell's every-64-allocations poll: at
        // roughly 9 ns per node the worst case a collector can wait behind
        // this loop is under a microsecond, and the cost is one relaxed
        // atomic load per 64 nodes.
        constexpr unsigned kPollInterval = 64;

        std::vector<const ProtoObject*> items;
        unsigned sincePoll = 0;
        for (const NodeCell* n = chain; n; n = n->next.load(std::memory_order_acquire)) {
            items.push_back(n->item);
            if (++sincePoll == kPollInterval) { sincePoll = 0; parkForStopTheWorld(context); }
        }

        // The chain is LIFO; FIFO out.  Hand-rolled rather than std::reverse
        // so that this second O(batch) pass polls too: at 400,000 items an
        // unpolled reverse is a six-figure-nanosecond hole in which the
        // collector cannot stop this thread, and constraint 1 is about the
        // shape of the curve, not about which of the two loops dominates.
        if (!items.empty()) {
            size_t lo = 0, hi = items.size() - 1;
            sincePoll = 0;
            while (lo < hi) {
                std::swap(items[lo++], items[hi--]);
                if (++sincePoll == kPollInterval) { sincePoll = 0; parkForStopTheWorld(context); }
            }
        }

        return context->newList(static_cast<unsigned>(items.size()), items.data());
    }

    const ProtoObject* ProtoMPSCQueue::asObject(ProtoContext*) const {
        return reinterpret_cast<const ProtoObject*>(this);   // already tagged
    }

    unsigned long ProtoMPSCQueue::getHash(ProtoContext* context) const {
        // Identity: the contents change under the caller's feet, so they
        // cannot contribute.  Cell::getHash is the cell address.
        return implOf(this)->Cell::getHash(context);
    }
}  // namespace proto
