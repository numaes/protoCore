// AttributeCasTests.cpp — ProtoObject::setAttributeIfEqual (compare-and-swap).
//
// Exercises the public attribute CAS: it must swap only when the current
// own-value still matches `expected`, treat `nullptr` as "attribute absent",
// reject immutable receivers, and — the property that lets embedders drop
// their mutexes — serialise concurrent read-modify-write sequences with no
// lost update.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include <thread>
#include <vector>

using namespace proto;

class AttributeCasTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* ctx = nullptr;

    void SetUp() override {
        space = new ProtoSpace();
        ctx = space->rootContext;
    }
    void TearDown() override { delete space; }

    const ProtoString* sym(const char* s) {
        return ProtoString::createSymbol(ctx, s);
    }
};

// A swap succeeds when the attribute still holds the expected value, and the
// new value is observable afterwards.
TEST_F(AttributeCasTest, SwapSucceedsWhenExpectedMatches) {
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    const ProtoString* key = sym("slot");
    const ProtoObject* a = ctx->fromInteger(10);
    const ProtoObject* b = ctx->fromInteger(20);

    obj->setAttribute(ctx, key, a);
    EXPECT_TRUE(obj->setAttributeIfEqual(ctx, key, a, b));
    EXPECT_EQ(obj->getAttribute(ctx, key), b);
}

// A swap fails — and writes nothing — when the attribute no longer holds the
// expected value.
TEST_F(AttributeCasTest, SwapFailsWhenExpectedMismatches) {
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    const ProtoString* key = sym("slot");
    const ProtoObject* a = ctx->fromInteger(10);
    const ProtoObject* b = ctx->fromInteger(20);
    const ProtoObject* wrong = ctx->fromInteger(99);

    obj->setAttribute(ctx, key, a);
    EXPECT_FALSE(obj->setAttributeIfEqual(ctx, key, wrong, b));
    EXPECT_EQ(obj->getAttribute(ctx, key), a) << "a failed CAS must not write";
}

// `expected == nullptr` means "install only if currently absent": it succeeds
// once on a fresh attribute and fails on every subsequent attempt.
TEST_F(AttributeCasTest, SwapWithExpectedAbsentInstallsOnce) {
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    const ProtoString* key = sym("slot");
    const ProtoObject* first = ctx->fromInteger(1);
    const ProtoObject* second = ctx->fromInteger(2);

    EXPECT_TRUE(obj->setAttributeIfEqual(ctx, key, nullptr, first));
    EXPECT_EQ(obj->getAttribute(ctx, key), first);
    // The attribute now exists, so an "absent" CAS must fail.
    EXPECT_FALSE(obj->setAttributeIfEqual(ctx, key, nullptr, second));
    EXPECT_EQ(obj->getAttribute(ctx, key), first);
}

// An immutable receiver cannot be updated in place — the CAS is a no-op that
// returns false.
TEST_F(AttributeCasTest, ImmutableReceiverReturnsFalse) {
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(false));
    const ProtoString* key = sym("slot");
    const ProtoObject* a = ctx->fromInteger(10);

    EXPECT_FALSE(obj->setAttributeIfEqual(ctx, key, nullptr, a));
    EXPECT_FALSE(obj->setAttributeIfEqual(ctx, key, a, ctx->fromInteger(20)));
}

// The core guarantee: many threads each running a CAS-retry increment must
// produce exactly NTHREADS * PER_THREAD increments — no update is ever lost.
// This is the lock-free read-modify-write that replaces an external mutex.
TEST_F(AttributeCasTest, ConcurrentCasIncrementLosesNoUpdate) {
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    const ProtoString* key = sym("counter");
    obj->setAttribute(ctx, key, ctx->fromInteger(0));

    const int NTHREADS  = 8;
    const int PER_THREAD = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < NTHREADS; ++t) {
        threads.emplace_back([&]() {
            ProtoContext threadCtx{space};
            for (int i = 0; i < PER_THREAD; ++i) {
                for (;;) {
                    const ProtoObject* old =
                        obj->getOwnAttributeDirect(&threadCtx, key);
                    const ProtoObject* next =
                        threadCtx.fromInteger(old->asLong(&threadCtx) + 1);
                    if (obj->setAttributeIfEqual(&threadCtx, key, old, next))
                        break;
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    const ProtoObject* finalValue = obj->getAttribute(ctx, key);
    EXPECT_EQ(finalValue->asLong(ctx),
              static_cast<long long>(NTHREADS) * PER_THREAD)
        << "a concurrent CAS-retry increment lost an update";
}
