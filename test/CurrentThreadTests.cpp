/*
 * CurrentThreadTests.cpp
 *
 * ProtoThread::getCurrentThread and ProtoSpace::getCurrentThread were
 * declared in protoCore.h but never defined, so any call failed at link time.
 * Linking this file is half the test.
 */

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <atomic>

using namespace proto;

namespace {

std::atomic<const ProtoThread*> gSeenStatic{nullptr};
std::atomic<const ProtoThread*> gSeenSpace{nullptr};
std::atomic<const ProtoThread*> gSeenNested{nullptr};

const ProtoObject* reportingBody(ProtoContext* c, const ProtoObject*, const ParentLink*,
                                 const ProtoArguments*, const ProtoKeywordArguments*) {
    gSeenStatic.store(ProtoThread::getCurrentThread(c));
    gSeenSpace.store(c->space->getCurrentThread(c));
    ProtoContext nested(c->space, c, nullptr, nullptr, nullptr, nullptr);
    gSeenNested.store(ProtoThread::getCurrentThread(&nested));
    return PROTO_NONE;
}

}  // namespace

TEST(CurrentThread, AThreadsBodySeesItsOwnThread) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;

    const ProtoThread* mainThread = ProtoThread::getCurrentThread(root);
    ASSERT_NE(mainThread, nullptr) << "the constructing thread is the space's main thread";
    EXPECT_EQ(space.getCurrentThread(root), mainThread);
    EXPECT_EQ(ProtoThread::getCurrentThread(nullptr), nullptr);

    const ProtoThread* t = space.newThread(root, ProtoString::createSymbol(root, "reporter"),
                                           reportingBody, nullptr, nullptr);
    ASSERT_NE(t, nullptr);
    const_cast<ProtoThread*>(t)->join(root);

    EXPECT_EQ(gSeenStatic.load(), t);
    EXPECT_EQ(gSeenSpace.load(), t);
    EXPECT_EQ(gSeenNested.load(), t) << "a nested context inherits its parent's thread";
    EXPECT_NE(gSeenStatic.load(), mainThread);
}
