// SelfHost.h -- protoCore's own reference implementation of Host, plus the
// deliberately non-conforming variants the self-check matrix needs.
//
// INTERNAL to protoCore: it is not installed and no embedder sees it.  Its job is
// rule 10 -- "a GC or concurrency test that cannot fail is worse than no test" --
// applied to this library itself.  Every case must be shown to FAIL against a
// host built to violate exactly that rule, or the case has stopped measuring
// anything and every green conformance report that relied on it is void.
#ifndef PROTO_CORE_CONFORMANCE_SELF_HOST_H
#define PROTO_CORE_CONFORMANCE_SELF_HOST_H

#include "../headers/protoCoreConformance.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace proto { namespace conformance {

/// Where a thread body and its user pointer are handed to a ProtoMethod, which
/// has a fixed signature and cannot carry a closure.  One probe runs at a time.
struct ThreadBridge
{
    static std::mutex&              mu()   { static std::mutex m; return m; }
    static Host::ThreadBody&        body() { static Host::ThreadBody b = nullptr; return b; }
    static void*&                   user() { static void* u = nullptr; return u; }
    static const Host::ThreadKind*& kind() { static const Host::ThreadKind* k = nullptr; return k; }

    /// The release flag for joinBlockingThread.
    static volatile bool*& release() { static volatile bool* r = nullptr; return r; }
};

/// A conforming Host implemented against protoCore directly.
///
/// It is by construction able to supply every capability, which is what makes it
/// the control: if any case reports NotApplicable under SelfHost, that case is
/// not finished, because the capability is unreachable and the case would be
/// NotApplicable for every runtime.
class SelfHost : public Host
{
public:
    SelfHost() {}
    ~SelfHost() override
    {
        shutdownQueueThreads();
        // The root set must go before the space, and the space is a member, so
        // this is the only correct place for it.
        if (mutableRoots_) { space_.destroyRootSet(mutableRoots_); mutableRoots_ = nullptr; }
    }

    const char*   name() const override { return "protoCore-selfhost"; }
    ProtoContext* mainContext() override { return space_.rootContext; }
    ProtoSpace&   space() { return space_; }

    // --- rule 1 -----------------------------------------------------------
    //
    // Allocate in a CHILD context and let it die.  Both submission paths are
    // exercised: the child's destructor submits its young chain, and the main
    // context's safepoint() submits whatever the loop charged to it.
    unsigned long makeGarbage(unsigned long requestedCells) override
    {
        return allocateAndDrop(requestedCells, /*asTuple=*/false);
    }

    // --- rule 5 -----------------------------------------------------------
    //
    // protoCore's own user-visible sequence is ProtoList, which is collectable.
    unsigned long makeSequenceGarbage(unsigned long requestedCells) override
    {
        return buildSequences(requestedCells, /*asTuple=*/false);
    }

    // --- rule 4 -----------------------------------------------------------
    const ProtoObject* internAttributeKey(const char* text) override
    {
        return reinterpret_cast<const ProtoObject*>(
            ProtoString::createSymbol(mainContext(), text));
    }

    // --- rules 2 and 11 ---------------------------------------------------
    bool forEachThreadKind(ThreadBody body, void* user) override
    {
        static const ThreadKind kind{"selfhost-worker", /*blocksWhenIdle=*/true,
                                     ThreadVerdict::Registered};
        std::lock_guard<std::mutex> g(ThreadBridge::mu());
        ThreadBridge::body() = body;
        ThreadBridge::user() = user;
        ThreadBridge::kind() = &kind;
        ProtoContext* ctx = mainContext();
        const ProtoThread* t = space_.newThread(
            ctx, ProtoString::createSymbol(ctx, "selfhost-worker"),
            &probeEntry, ctx->newList(), nullptr);
        if (!t) return false;
        const_cast<ProtoThread*>(t)->join(ctx);
        return true;
    }

    // --- rules 3 and 8 ----------------------------------------------------
    bool runProducerConsumer(unsigned long units) override
    {
        ProtoContext* ctx = mainContext();
        // Rule 3, obeyed by the harness that audits it.  The queue and the
        // message under construction are held across allocating calls
        // (newList, appendLast, push), so a bare C++ local would be invisible to
        // the marker and freed under us.  The first draft of this method DID
        // hold them in bare locals, and heap.ceiling_progress aborted the
        // process with protoCore's "CRITICAL TAGGED POINTER (Phase4(mark))"
        // under a hard ceiling -- which is the case earning its place against
        // the library's own reference host.  Both are pinned in automatic-local
        // slots of a child context, which GC Phase 2 scans.
        ProtoContext work(&space_, ctx);
        work.resizeAutomaticLocals(4);
        const ProtoMPSCQueue* q = work.newMPSCQueue();
        if (!q) return false;
        work.setAutomaticLocal(kSlotQueue, q->asObject(&work));

        unsigned long drained = 0;
        for (unsigned long i = 0; i < units; ++i) {
            const ProtoList* msg = work.newList();
            work.setAutomaticLocal(kSlotMessage, msg->asObject(&work));
            msg = msg->appendLast(&work, work.fromLong((long long) i));
            work.setAutomaticLocal(kSlotMessage, msg->asObject(&work));
            msg = msg->appendLast(&work, work.fromLong((long long) (i * 3 + 1)));
            work.setAutomaticLocal(kSlotMessage, msg->asObject(&work));
            q->push(&work, msg->asObject(&work));
            work.setAutomaticLocal(kSlotMessage, PROTO_NONE);
            if ((i & 0x3F) == 0) {
                const ProtoList* taken = q->takeAll(&work);
                if (taken) {
                    work.setAutomaticLocal(kSlotTaken, taken->asObject(&work));
                    drained += taken->getSize(&work);
                    work.setAutomaticLocal(kSlotTaken, PROTO_NONE);
                }
                work.safepoint();
            }
        }
        const ProtoList* taken = q->takeAll(&work);
        if (taken) {
            work.setAutomaticLocal(kSlotTaken, taken->asObject(&work));
            drained += taken->getSize(&work);
            work.setAutomaticLocal(kSlotTaken, PROTO_NONE);
        }
        work.safepoint();
        return drained == units;
    }

    // --- rule 2b ----------------------------------------------------------
    //
    // Start a managed thread that waits for the flag, then join it through
    // protoCore's own ProtoThread::join -- the API whose bracketing is the
    // kernel's job as of 2026-09-25.
    bool joinBlockingThread(volatile bool* releaseFlag) override
    {
        std::lock_guard<std::mutex> g(ThreadBridge::mu());
        ThreadBridge::release() = releaseFlag;
        ProtoContext* ctx = mainContext();
        const ProtoThread* t = space_.newThread(
            ctx, ProtoString::createSymbol(ctx, "selfhost-waiter"),
            &waiterEntry, ctx->newList(), nullptr);
        if (!t) return false;
        const_cast<ProtoThread*>(t)->join(ctx);
        return true;
    }

    unsigned long externalBytesAccounted() override { return externalBytes_; }

    // --- rule 13 ---------------------------------------------------------
    //
    // protoCore's own mutable graph, as an embedder would build it: a mutable
    // "class" with methods installed on it, and mutable instances that are its
    // children and name it back.  Every edge here points from an instance to
    // the class and never returns, so it is ACYCLIC -- and it is the shape that
    // must not be reported, because it is the commonest shape in the family
    // (class prototypes in protoScala and protoST, and every instance of them).
    unsigned long makeMutableGraph() override
    {
        ProtoContext* ctx = mainContext();
        const ProtoString* kName   = ProtoString::createSymbol(ctx, "selfhost_class_name");
        const ProtoString* kOwner  = ProtoString::createSymbol(ctx, "selfhost_owner");
        const ProtoString* kSerial = ProtoString::createSymbol(ctx, "selfhost_serial");

        const ProtoObject* klass = ctx->newObject(/*mutableObject=*/true);
        // The first write is what publishes the table entry at all.
        klass = klass->setAttribute(ctx, kName,
                     reinterpret_cast<const ProtoObject*>(
                         ProtoString::createSymbol(ctx, "SelfHostPoint")));
        unsigned long built = 1;
        for (unsigned long i = 0; i < 16; ++i) {
            const ProtoObject* inst = klass->newChild(ctx, /*isMutable=*/true);
            inst->setAttribute(ctx, kOwner, klass);          // instance -> class
            inst->setAttribute(ctx, kSerial, ctx->fromLong((long) i));
            ++built;
        }
        // Pinned in a ProtoRootSet, not held in a C++ member: a bare member is
        // invisible to the marker (rule 3), and a reference host that broke the
        // rule it audits would be the worst possible control.
        pinMutableRoot(klass);
        return built;
    }

    /// The control declares an acyclic mutable graph, so any cycle the scan
    /// finds against it is a Fail -- which is what makes the mutation below a
    /// real mutation.
    long declaredMutableCycles() override { return 0; }

    /// The control supplies every capability.  protoCore keeps two identities
    /// that differ only in provider distinct, so the honest answer is 1.
    int loadSamePathTwoProviders() override
    {
        const ModuleIdentity a("selfhost.provider.A", "selfhost/same", "1.0.0");
        const ModuleIdentity b("selfhost.provider.B", "selfhost/same", "1.0.0");
        ProtoContext* ctx = mainContext();
        const ProtoObject* ma = space_.registerModule(a, ctx->newObject(false));
        const ProtoObject* mb = space_.registerModule(b, ctx->newObject(false));
        return (ma && mb && ma != mb) ? 1 : 0;
    }

protected:
    /// The workload, shared by the conforming and non-conforming variants.
    /// `longLivedContext` is the rule-1 mutation knob: allocate everything into
    /// one context that never ends and never call safepoint(), and the young
    /// chain is never submitted.
    unsigned long allocateAndDrop(unsigned long requestedCells, bool asTuple,
                                  bool longLivedContext = false,
                                  bool callSafepoint = true)
    {
        ProtoContext* main = mainContext();
        const ProtoString* k1 = ProtoString::createSymbol(main, "selfhost_key_one");
        const ProtoString* k2 = ProtoString::createSymbol(main, "selfhost_key_two");
        const unsigned long kPerBatch = 2000;
        // Loop until the SPACE's own in-use figure has grown past the request,
        // rather than guessing a cells-per-object constant.  A guess that is too
        // low makes the case report NotApplicable ("the workload did not consume
        // enough heap to judge"), which is how a reference host silently stops
        // being a control.
        const long floor = inUse() + (long) (requestedCells + requestedCells / 2);
        const unsigned long kMaxBatches = 4096;
        unsigned long objects = 0;

        for (unsigned long b = 0; b < kMaxBatches && inUse() < floor; ++b) {
            if (longLivedContext) {
                for (unsigned long i = 0; i < kPerBatch; ++i)
                    makeOne(main, k1, k2, (long long) i, asTuple);
                objects += kPerBatch;
                // Deliberately no safepoint(): the young chain accumulates on a
                // context that never dies, so nothing is ever submitted.
            } else {
                ProtoContext child(&space_, main);
                for (unsigned long i = 0; i < kPerBatch; ++i)
                    makeOne(&child, k1, k2, (long long) i, asTuple);
                objects += kPerBatch;
            }
            if (callSafepoint) main->safepoint();
        }
        return objects;
    }

    unsigned long buildSequences(unsigned long requestedCells, bool asTuple)
    {
        ProtoContext* main = mainContext();
        const unsigned long kPerSeq = 16;
        unsigned long built = 0;
        const unsigned long kPerBatch = 1000;
        const long floor = inUse() + (long) (requestedCells + requestedCells / 2);
        const unsigned long kMaxBatches = 4096;

        for (unsigned long b = 0; b < kMaxBatches && inUse() < floor; ++b) {
            ProtoContext child(&space_, main);
            for (unsigned long s = 0; s < kPerBatch; ++s) {
                if (asTuple) {
                    // The rule-5 mutation: every interned tuple node is
                    // perennial, so this grows the heap monotonically.
                    std::vector<const ProtoObject*> els;
                    els.reserve(kPerSeq);
                    for (unsigned long i = 0; i < kPerSeq; ++i)
                        els.push_back(child.fromLong((long long) (b * 7919 + s * 131 + i)));
                    const ProtoTuple* t = child.newTuple(els);
                    (void) t;
                } else {
                    const ProtoList* l = child.newList();
                    for (unsigned long i = 0; i < kPerSeq; ++i)
                        l = l->appendLast(&child, child.fromLong((long long) (b * 7919 + s * 131 + i)));
                    (void) l;
                }
                ++built;
            }
            main->safepoint();
        }
        return built;
    }

    void shutdownQueueThreads() {}

    /// Cells the space has taken from the OS and not got back on its free list.
    /// The same figure CycleDriver measures, used here only so a workload can
    /// tell when it has allocated enough to be judged.
    long inUse() const
    {
        return (long) space_.heapSize - (long) space_.freeCellsCount;
    }

    static constexpr unsigned kSlotQueue   = 0;
    static constexpr unsigned kSlotMessage = 1;
    static constexpr unsigned kSlotTaken   = 2;

    ProtoSpace    space_;
    unsigned long externalBytes_ = 0;
    /// Keeps rule 13's graph reachable for the rest of the run, so the scan
    /// sees a live graph and not one in the middle of being released -- pinned
    /// as a real GC root, which is what rule 3 demands of everything else.
    void pinMutableRoot(const ProtoObject* obj)
    {
        if (!mutableRoots_) mutableRoots_ = space_.createRootSet("selfhost-rule13");
        if (mutableRoots_) mutableRoots_->add(obj);
    }
    ProtoRootSet* mutableRoots_ = nullptr;

private:
    static void makeOne(ProtoContext* ctx, const ProtoString* k1,
                        const ProtoString* k2, long long i, bool asTuple)
    {
        if (asTuple) {
            std::vector<const ProtoObject*> els{ctx->fromLong(i), ctx->fromLong(i + 1)};
            (void) ctx->newTuple(els);
            return;
        }
        const ProtoObject* o = ctx->newObject(/*mutableObject=*/false);
        o = o->setAttribute(ctx, k1, ctx->fromLong(i));
        o = o->setAttribute(ctx, k2, ctx->fromLong(i + 1));
        (void) o;
    }

    static const ProtoObject* probeEntry(ProtoContext* context,
                                         const ProtoObject* /*self*/,
                                         const ParentLink* /*pl*/,
                                         const ProtoList* /*args*/,
                                         const ProtoSparseList* /*kwargs*/)
    {
        Host::ThreadBody b = ThreadBridge::body();
        if (b && ThreadBridge::kind())
            b(ThreadBridge::user(), *ThreadBridge::kind(), context);
        return PROTO_NONE;
    }

    static const ProtoObject* waiterEntry(ProtoContext* context,
                                          const ProtoObject* /*self*/,
                                          const ParentLink* /*pl*/,
                                          const ProtoList* /*args*/,
                                          const ProtoSparseList* /*kwargs*/)
    {
        volatile bool* flag = ThreadBridge::release();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (flag && !*flag && std::chrono::steady_clock::now() < deadline) {
            // Cooperate: this thread is registered and counted in
            // runningThreads, so a poll loop without a safepoint would hold the
            // quorum itself and the case would be measuring the wrong thread.
            context->safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return PROTO_NONE;
    }
};

//===========================================================================
// The mutation matrix.  Each variant breaks exactly one rule.
//===========================================================================

/// Rule 1 -- protoST's S15 exactly: allocate into a long-lived context and never
/// call safepoint(), so the young chain is never submitted and is live by
/// construction.
class NonConformingHost_NoSafepoint final : public SelfHost
{
public:
    const char* name() const override { return "mutant-no-safepoint"; }
    unsigned long makeGarbage(unsigned long requestedCells) override
    {
        return allocateAndDrop(requestedCells, /*asTuple=*/false,
                               /*longLivedContext=*/true, /*callSafepoint=*/false);
    }
};

/// Rule 5 -- protoClojure's pre-Track-C vectors: build the runtime's sequences
/// out of ProtoTuple, every node of which is interned and perennial.
class NonConformingHost_PerennialSequence final : public SelfHost
{
public:
    const char* name() const override { return "mutant-perennial-sequence"; }
    unsigned long makeSequenceGarbage(unsigned long requestedCells) override
    {
        return buildSequences(requestedCells, /*asTuple=*/true);
    }
};

/// Rule 11 -- a ProtoContext on a bare std::thread: never in space->threads, so
/// never root-scanned, yet it declares itself Registered.
class NonConformingHost_RawStdThread final : public SelfHost
{
public:
    const char* name() const override { return "mutant-raw-std-thread"; }
    bool forEachThreadKind(ThreadBody body, void* user) override
    {
        static const ThreadKind kind{"raw-std-thread", /*blocksWhenIdle=*/false,
                                     ThreadVerdict::Registered};
        ProtoSpace* sp = &space_;
        std::thread t([body, user, sp]() {
            ProtoContext threadCtx{sp};
            body(user, kind, &threadCtx);
        });
        t.join();
        return true;
    }
};

/// Rule 11's other half -- a kind that declares it holds nothing and then
/// allocates.  A declaration the code contradicts.
class NonConformingHost_LyingHoldsNothing final : public SelfHost
{
public:
    const char* name() const override { return "mutant-lying-holds-nothing"; }
    bool forEachThreadKind(ThreadBody body, void* user) override
    {
        static const ThreadKind kind{"lying-pool-thread", /*blocksWhenIdle=*/true,
                                     ThreadVerdict::HoldsNothing};
        std::lock_guard<std::mutex> g(ThreadBridge::mu());
        ThreadBridge::body() = body;
        ThreadBridge::user() = user;
        ThreadBridge::kind() = &kind;
        ProtoContext* ctx = mainContext();
        const ProtoThread* t = space_.newThread(
            ctx, ProtoString::createSymbol(ctx, "lying-pool-thread"),
            &allocThenProbe, ctx->newList(), nullptr);
        if (!t) return false;
        const_cast<ProtoThread*>(t)->join(ctx);
        return true;
    }
private:
    static const ProtoObject* allocThenProbe(ProtoContext* context,
                                             const ProtoObject*,
                                             const ParentLink*,
                                             const ProtoList*,
                                             const ProtoSparseList*)
    {
        // Allocate on this thread's own context, contradicting HoldsNothing.
        for (int i = 0; i < 64; ++i) (void) context->newObject(false);
        Host::ThreadBody b = ThreadBridge::body();
        if (b && ThreadBridge::kind())
            b(ThreadBridge::user(), *ThreadBridge::kind(), context);
        return PROTO_NONE;
    }
};

/// Rule 2b -- a registered thread joined with a bare std::thread::join, which
/// reaches no safepoint and holds the stop-the-world quorum.  This is the shape
/// the kernel's own ProtoThread::join no longer has, and which a runtime can
/// still write for itself.
class NonConformingHost_BareJoin final : public SelfHost
{
public:
    const char* name() const override { return "mutant-bare-join"; }
    bool joinBlockingThread(volatile bool* releaseFlag) override
    {
        // A registered thread must exist and be counted, or the quorum is met
        // trivially and the mutation proves nothing.  The joined thread is the
        // one below; the bare join is on THIS thread, which is the main thread
        // and is counted in runningThreads from ProtoSpace construction.
        std::thread t([releaseFlag]() {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (releaseFlag && !*releaseFlag &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        });
        // No UnmanagedScope: the calling thread stays in the running set while
        // it blocks, so parkedThreads can never reach runningThreads.
        t.join();
        return true;
    }
};

/// Rule 4 -- an attribute key built with fromUTF8String instead of
/// createSymbol.  Correct through getAttribute's content fallback, silently
/// absent through the getOwnAttributeDirect fast path.
class NonConformingHost_UninternedKey final : public SelfHost
{
public:
    const char* name() const override { return "mutant-uninterned-key"; }
    const ProtoObject* internAttributeKey(const char* text) override
    {
        return mainContext()->fromUTF8String(text);
    }
};

/// Rule 13 -- a cycle among mutables, in the shape protoScala's captured `var`
/// has: the cell is a protoCore mutable, its current value is the closure, and
/// the closure captured the cell.  Value -> handle, and the entry can never be
/// released.  The host still declares ZERO structural cycles, so the case must
/// Fail: an undeclared cycle is unaccounted permanent retention.
class NonConformingHost_MutableCycle final : public SelfHost
{
public:
    const char* name() const override { return "mutant-mutable-cycle"; }
    unsigned long makeMutableGraph() override
    {
        // The acyclic part first, so the finding is not merely "the only two
        // mutables in the table refer to each other".
        const unsigned long acyclic = SelfHost::makeMutableGraph();

        ProtoContext* ctx = mainContext();
        const ProtoString* kValue =
            ProtoString::createSymbol(ctx, "selfhost_cell_value");
        const ProtoString* kCapture =
            ProtoString::createSymbol(ctx, "selfhost_captured_cell");

        const ProtoObject* cell    = ctx->newObject(/*mutableObject=*/true);
        const ProtoObject* closure = ctx->newObject(/*mutableObject=*/true);
        closure->setAttribute(ctx, kCapture, cell);   // the closure captured it
        cell->setAttribute(ctx, kValue, closure);     // and the cell holds it
        pinMutableRoot(closure);
        return acyclic + 2;
    }
    long declaredMutableCycles() override { return 0; }
};

/// A Host that implements only the three required capabilities, and those
/// minimally.  Nothing it is asked for can be supplied, so nothing may be
/// reported as a Pass.  This is the assertion that the suite cannot repeat
/// protoST's mistake in its own voice.
class EmptyHost final : public Host
{
public:
    const char*   name() const override { return "empty-host"; }
    ProtoContext* mainContext() override { return space_.rootContext; }
    unsigned long makeGarbage(unsigned long) override { return 0; }
private:
    ProtoSpace space_;
};

}}  // namespace proto::conformance

#endif
