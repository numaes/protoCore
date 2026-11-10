// NewThreadMainContextTests.cpp -- ProtoSpace::newThread touches no other
// thread's root slot.
//
// Through 3.5.1 newThread built the new thread's root context with the
// general ProtoContext constructor and `previous == nullptr`.  That
// constructor registered it as ProtoSpace::mainContext (on a worker) or as
// the main thread's current context (on the main thread), and newThread then
// put the snapshotted values back.  The snapshot, the write and the restore
// were plain accesses: two threads calling newThread at once could leave
// space->mainContext on one worker's root context, which the collector then
// scanned as the main thread's roots and which dangled once that worker
// exited.  ThreadSanitizer found it in protoST, whose worker pool grows from
// worker threads.
//
// Since 3.5.2 the root context is built with ProtoContext's ThreadRootTag
// constructor, bound to the new thread from the start: newThread writes
// neither space->mainContext nor the creating thread's current context.
//
//   * NewThreadMainContext.ThreadRootConstructorRegistersOnlyItsThread checks
//     the constructor's contract directly, on a worker, with the main context
//     and the worker's own current context known: neither changes while the
//     context is built, lives and is destroyed.  No timing involved.
//   * NewThreadMainContext.ChildRootContextIsBoundToTheChild checks, inside
//     each child, that its root context is its own thread's and is not the
//     main context, for threads created by the main thread and by a worker.
//   * NewThreadMainContext.ConcurrentCreatorsLeaveTheMainContextAlone has
//     several workers create threads at the same time.  Under
//     ThreadSanitizer (the CI job runs every test) the 3.5.1 code reports
//     the race on space->mainContext; without it, the test still checks that
//     the main context and each creator's current context are unchanged
//     afterwards, which a lost restore breaks.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace proto;

namespace {

ProtoContext* currentContextOf(const ProtoThread* thread) {
    return toImpl<const ProtoThreadImplementation>(thread)->context;
}

// --- Child bookkeeping -----------------------------------------------------

std::atomic<int> g_childrenRun{0};
std::atomic<int> g_childrenBad{0};
std::atomic<ProtoContext*> g_mainContext{nullptr};

const ProtoObject* checkingChild(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                 const ProtoArguments*, const ProtoKeywordArguments*) {
    // thread_main may run the method in a context chained to the root one;
    // walk to the root.
    ProtoContext* root = ctx;
    while (root->previous) root = root->previous;
    const bool ok = ctx->thread != nullptr && root->thread == ctx->thread &&
                    root != g_mainContext.load() && root != ctx->space->rootContext;
    if (!ok) g_childrenBad.fetch_add(1);
    // A little allocation, so the child's context is really used.
    const ProtoList* l = ctx->newList();
    for (int i = 0; i < 20; ++i) l = l->appendLast(ctx, ctx->fromInteger(i));
    if (l->getSize(ctx) != 20) g_childrenBad.fetch_add(1);
    g_childrenRun.fetch_add(1);
    return PROTO_NONE;
}

// --- Creator workers -------------------------------------------------------

struct CreatorState {
    int childrenPerCreator = 0;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> mainContextChanged{0};
    std::atomic<int> ownContextChanged{0};
    std::atomic<int> done{0};
};
CreatorState* g_creators = nullptr;

const ProtoObject* creatorWorker(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                 const ProtoArguments*, const ProtoKeywordArguments*) {
    CreatorState& st = *g_creators;
    ProtoSpace* space = ctx->space;
    ProtoContext* const ownBefore = currentContextOf(ctx->thread);
    st.ready.fetch_add(1);
    {
        // Wait unmanaged, so collections are not held up by the barrier.
        ProtoContext::UnmanagedScope out(ctx);
        while (!st.go.load()) std::this_thread::yield();
    }
    std::vector<const ProtoThread*> children;
    for (int i = 0; i < st.childrenPerCreator; ++i) {
        children.push_back(space->newThread(ctx, ProtoString::createSymbol(ctx, "child"),
                                            checkingChild, nullptr, nullptr));
        // Read through the same lock the creators synchronise on, so the
        // check itself is no data race.
        ProtoContext* mainNow;
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            mainNow = space->mainContext;
        }
        if (mainNow != g_mainContext.load()) st.mainContextChanged.fetch_add(1);
        if (currentContextOf(ctx->thread) != ownBefore) st.ownContextChanged.fetch_add(1);
    }
    for (const ProtoThread* t : children) const_cast<ProtoThread*>(t)->join(ctx);
    st.done.fetch_add(1);
    return PROTO_NONE;
}

// --- Constructor contract on a worker -------------------------------------

struct RootCtorState {
    std::atomic<bool> checked{false};
    std::atomic<bool> ok{false};
};
RootCtorState* g_rootCtor = nullptr;

const ProtoObject* rootCtorWorker(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                  const ProtoArguments*, const ProtoKeywordArguments*) {
    ProtoSpace* space = ctx->space;
    ProtoContext* const mainBefore = space->mainContext;
    ProtoContext* const ownBefore = currentContextOf(ctx->thread);
    bool ok = true;
    {
        // Bind the new root context to THIS worker's own thread: the only
        // slot it may write is this thread's current context.
        ProtoContext root(ProtoContext::ThreadRootTag{}, space, ctx->thread);
        ok = ok && space->mainContext == mainBefore;
        ok = ok && root.thread == ctx->thread && root.previous == nullptr;
        ok = ok && currentContextOf(ctx->thread) == &root;
        ok = ok && root.getAutomaticLocalsCount() == 0;
        const ProtoList* l = root.newList()->appendLast(&root, root.fromInteger(7));
        ok = ok && l->getAt(&root, 0)->asLong(&root) == 7;
        ok = ok && space->mainContext == mainBefore;
    }
    // Destroying it pops this thread's slot back to its previous (nullptr),
    // as a thread's root context does at exit; restore the worker's own.
    ok = ok && space->mainContext == mainBefore;
    toImpl<ProtoThreadImplementation>(ctx->thread)->implSetCurrentContext(ownBefore);
    g_rootCtor->ok = ok;
    g_rootCtor->checked = true;
    return PROTO_NONE;
}

void waitUnmanaged(ProtoContext* ctx, const std::atomic<bool>& flag) {
    ProtoContext::UnmanagedScope out(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace

TEST(NewThreadMainContext, ThreadRootConstructorRegistersOnlyItsThread) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    RootCtorState st;
    g_rootCtor = &st;
    const ProtoThread* w = space.newThread(root, ProtoString::createSymbol(root, "root-ctor"),
                                           rootCtorWorker, nullptr, nullptr);
    waitUnmanaged(root, st.checked);
    {
        ProtoContext::UnmanagedScope out(root);
        const_cast<ProtoThread*>(w)->join(root);
    }
    g_rootCtor = nullptr;
    ASSERT_TRUE(st.checked.load());
    EXPECT_TRUE(st.ok.load())
        << "the ThreadRootTag constructor wrote a slot other than its own thread's";
}

TEST(NewThreadMainContext, ChildRootContextIsBoundToTheChild) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    g_mainContext = space.mainContext;
    g_childrenRun = 0;
    g_childrenBad = 0;

    // Created by the main thread.
    ProtoContext* const mainBefore = space.mainContext;
    ProtoContext* const ownBefore = currentContextOf(root->thread);
    const ProtoThread* t = space.newThread(root, ProtoString::createSymbol(root, "child"),
                                           checkingChild, nullptr, nullptr);
    EXPECT_EQ(space.mainContext, mainBefore);
    EXPECT_EQ(currentContextOf(root->thread), ownBefore);
    {
        ProtoContext::UnmanagedScope out(root);
        const_cast<ProtoThread*>(t)->join(root);
    }

    // Created by a worker.
    CreatorState st;
    st.childrenPerCreator = 8;
    g_creators = &st;
    const ProtoThread* w = space.newThread(root, ProtoString::createSymbol(root, "creator"),
                                           creatorWorker, nullptr, nullptr);
    {
        ProtoContext::UnmanagedScope out(root);
        while (st.ready.load() < 1) std::this_thread::yield();
        st.go = true;
        const_cast<ProtoThread*>(w)->join(root);
    }
    g_creators = nullptr;
    EXPECT_EQ(st.done.load(), 1);
    EXPECT_EQ(st.mainContextChanged.load(), 0);
    EXPECT_EQ(st.ownContextChanged.load(), 0);
    EXPECT_EQ(g_childrenRun.load(), 1 + 8);
    EXPECT_EQ(g_childrenBad.load(), 0);
    EXPECT_EQ(space.mainContext, mainBefore);
}

TEST(NewThreadMainContext, ConcurrentCreatorsLeaveTheMainContextAlone) {
    constexpr int kCreators = 4;
    constexpr int kChildren = 12;
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    g_mainContext = space.mainContext;
    g_childrenRun = 0;
    g_childrenBad = 0;
    CreatorState st;
    st.childrenPerCreator = kChildren;
    g_creators = &st;
    ProtoContext* const mainBefore = space.mainContext;

    std::vector<const ProtoThread*> creators;
    for (int i = 0; i < kCreators; ++i)
        creators.push_back(space.newThread(root, ProtoString::createSymbol(root, "creator"),
                                           creatorWorker, nullptr, nullptr));
    {
        ProtoContext::UnmanagedScope out(root);
        while (st.ready.load() < kCreators) std::this_thread::yield();
        st.go = true;   // every creator calls newThread at the same time
        for (const ProtoThread* c : creators) const_cast<ProtoThread*>(c)->join(root);
    }
    g_creators = nullptr;
    EXPECT_EQ(st.done.load(), kCreators);
    EXPECT_EQ(st.mainContextChanged.load(), 0)
        << "space->mainContext changed while threads were being created";
    EXPECT_EQ(st.ownContextChanged.load(), 0)
        << "creating a thread changed the creator's current context";
    EXPECT_EQ(g_childrenRun.load(), kCreators * kChildren);
    EXPECT_EQ(g_childrenBad.load(), 0);
    EXPECT_EQ(space.mainContext, mainBefore);
    // The main context is still a usable root: a collection and an
    // allocation through it work.
    space.triggerGC();
    const ProtoList* l = root->newList()->appendLast(root, root->fromInteger(1));
    EXPECT_EQ(l->getSize(root), 1u);
}
