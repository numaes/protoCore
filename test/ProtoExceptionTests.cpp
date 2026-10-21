// ProtoExceptionTests.cpp — proto::ProtoException and protoCore's own errors.
//
// The model (docs/EXCEPTIONS.md): every language on protoCore throws one C++
// type, ProtoException, carrying the thrown value; the exception's type is
// the value's prototype chain.  protoCore's own failures are raised through
// the space's error factory, never as C++ standard exceptions.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

using namespace proto;

namespace {

void requestCycle(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    space.gcStarted = true;
    space.gcCV.notify_all();
}

// Runs collection cycles until `cycles` more have completed, parked so the
// collector can stop the world.
bool runCycles(ProtoSpace& space, ProtoContext* ctx, uint64_t cycles) {
    ProtoContext::UnmanagedScope parked(ctx);
    const uint64_t target = space.getGCCycleCount() + cycles;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (space.getGCCycleCount() < target) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        requestCycle(space);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

const ProtoString* sym(ProtoContext* ctx, const char* s) {
    return ProtoString::createSymbol(ctx, s);
}

std::string messageOf(ProtoContext* ctx, const ProtoObject* error) {
    const ProtoObject* m = error->getAttribute(ctx, sym(ctx, "message"));
    std::string out;
    if (m && m != PROTO_NONE && m->isString(ctx)) out = m->asString(ctx)->toStdString(ctx);
    return out;
}

class ProtoExceptionTest : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
};

}  // namespace

// A raised value arrives unchanged, and the handler selects by prototype.
TEST_F(ProtoExceptionTest, RaiseCarriesTheValueAndMatchesByPrototype) {
    const ProtoObject* moduleError = ctx->newObject(false);
    const ProtoObject* notFound = moduleError->newChild(ctx);
    const ProtoObject* unrelated = ctx->newObject(false);
    const ProtoObject* thrown = notFound->newChild(ctx);

    try {
        ctx->raise(thrown);
        FAIL() << "raise returned";
    } catch (const ProtoException& e) {
        EXPECT_EQ(e.value(), thrown);
        EXPECT_TRUE(e.isInstanceOf(ctx, notFound));
        EXPECT_TRUE(e.isInstanceOf(ctx, moduleError));
        EXPECT_FALSE(e.isInstanceOf(ctx, unrelated));
        EXPECT_FALSE(e.isInstanceOf(ctx, PROTO_NONE));
    }
}

// Any value may be thrown, tagged values included.
TEST_F(ProtoExceptionTest, TaggedValuesAreThrownUnpinned) {
    try {
        ctx->raise(ctx->fromInteger(42));
    } catch (const ProtoException& e) {
        EXPECT_EQ(e.value()->asLong(ctx), 42);
    }
    try {
        throw ProtoException(ctx, nullptr);
    } catch (const ProtoException& e) {
        EXPECT_EQ(e.value(), PROTO_NONE);
    }
}

// A language value is not a C++ failure: a std::exception handler does not
// see it.
TEST_F(ProtoExceptionTest, NotCaughtAsStdException) {
    bool sawStd = false;
    bool sawProto = false;
    try {
        try {
            ctx->raise(ctx->newObject(false));
        } catch (const std::exception&) {
            sawStd = true;
        }
    } catch (const ProtoException&) {
        sawProto = true;
    }
    EXPECT_FALSE(sawStd);
    EXPECT_TRUE(sawProto);
}

// A derived exception (a runtime's own, with its extras) is caught as the base.
TEST_F(ProtoExceptionTest, RuntimeSubclassIsCaughtAsTheBase) {
    struct RuntimeException : ProtoException {
        RuntimeException(ProtoContext* c, const ProtoObject* v, int l)
            : ProtoException(c, v), line(l) {}
        int line;
    };
    const ProtoObject* v = ctx->newObject(false);
    try {
        throw RuntimeException(ctx, v, 7);
    } catch (const ProtoException& e) {
        EXPECT_EQ(e.value(), v);
        EXPECT_EQ(dynamic_cast<const RuntimeException&>(e).line, 7);
    }
}

// The value is pinned while the exception is in flight: it survives
// collections after the context that built it is gone, and the pin is
// released with the last copy.
TEST_F(ProtoExceptionTest, ValueSurvivesCollectionWhileInFlight) {
    std::exception_ptr inFlight;
    {
        ProtoContext child(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* v = child.newObject(false)->setAttribute(
            &child, sym(&child, "message"), child.fromUTF8String("kept alive by the pin"));
        try {
            child.raise(v);
        } catch (const ProtoException&) {
            inFlight = std::current_exception();
        }
    }
    // Garbage to give the collector something to sweep around the value.
    for (int i = 0; i < 20000; ++i) {
        ProtoContext scratch(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        (void) scratch.newObject(false)->setAttribute(&scratch, sym(&scratch, "i"), scratch.fromInteger(i));
    }
    ASSERT_TRUE(runCycles(space, ctx, 2));
    try {
        std::rethrow_exception(inFlight);
    } catch (const ProtoException& e) {
        EXPECT_EQ(messageOf(ctx, e.value()), "kept alive by the pin");
    }
    inFlight = nullptr;
    EXPECT_EQ(space.exceptionRoots_->size(), 0u);
}

// protoCore's own errors, with the default factory: a child of the kind's
// prototype, itself a child of ProtoCoreError, with the message.
TEST_F(ProtoExceptionTest, ProtoCoreErrorsUseTheDefaultFactory) {
    const ProtoObject* root = space.getErrorRootPrototype(ctx);
    const ProtoObject* divZero = space.getErrorPrototype(ctx, ProtoErrorKind::DivisionByZero);
    const ProtoObject* overflow = space.getErrorPrototype(ctx, ProtoErrorKind::Overflow);
    try {
        (void) ctx->fromInteger(5)->divide(ctx, ctx->fromInteger(0));
        FAIL() << "division by zero returned";
    } catch (const ProtoException& e) {
        EXPECT_TRUE(e.isInstanceOf(ctx, divZero));
        EXPECT_TRUE(e.isInstanceOf(ctx, root));
        EXPECT_FALSE(e.isInstanceOf(ctx, overflow));
        EXPECT_EQ(messageOf(ctx, e.value()), "Division by zero.");
    }
    const ProtoObject* name = divZero->getAttribute(ctx, sym(ctx, "name"));
    EXPECT_EQ(name->asString(ctx)->toStdString(ctx), "DivisionByZero");
}

TEST_F(ProtoExceptionTest, EveryKindHasItsPrototype) {
    const ProtoObject* root = space.getErrorRootPrototype(ctx);
    for (int k = 0; k < kProtoErrorKindCount; ++k) {
        const auto kind = static_cast<ProtoErrorKind>(k);
        const ProtoObject* p = space.getErrorPrototype(ctx, kind);
        EXPECT_EQ(p->isInstanceOf(ctx, root), PROTO_TRUE) << ProtoSpace::errorKindName(kind);
        EXPECT_EQ(p->getAttribute(ctx, sym(ctx, "name"))->asString(ctx)->toStdString(ctx),
                  ProtoSpace::errorKindName(kind));
    }
}

// The prototypes outlive collections: they are pinned for the space's life.
TEST_F(ProtoExceptionTest, ErrorPrototypesSurviveCollection) {
    const ProtoObject* p = space.getErrorPrototype(ctx, ProtoErrorKind::TypeMismatch);
    ASSERT_TRUE(runCycles(space, ctx, 2));
    EXPECT_EQ(space.getErrorPrototype(ctx, ProtoErrorKind::TypeMismatch), p);
    EXPECT_EQ(p->getAttribute(ctx, sym(ctx, "name"))->asString(ctx)->toStdString(ctx), "TypeMismatch");
}

// A runtime's factory decides the value: protoCore's errors arrive as the
// language's own exceptions.
namespace {
const ProtoObject* g_languageZeroDivision = nullptr;
ProtoErrorKind g_lastKind = ProtoErrorKind::InvalidArgument;

const ProtoObject* languageFactory(ProtoContext* context, ProtoErrorKind kind, const char* message) {
    g_lastKind = kind;
    const ProtoObject* proto = kind == ProtoErrorKind::DivisionByZero
        ? g_languageZeroDivision
        : context->space->getErrorPrototype(context, kind);
    return proto->newChild(context)->setAttribute(
        context, ProtoString::createSymbol(context, "message"), context->fromUTF8String(message));
}
}  // namespace

TEST_F(ProtoExceptionTest, InstalledFactoryBuildsTheValue) {
    g_languageZeroDivision = ctx->newObject(false);
    space.setErrorFactory(languageFactory);
    EXPECT_EQ(space.getErrorFactory(), &languageFactory);
    try {
        (void) ctx->fromInteger(1)->modulo(ctx, ctx->fromInteger(0));
        FAIL();
    } catch (const ProtoException& e) {
        EXPECT_EQ(g_lastKind, ProtoErrorKind::DivisionByZero);
        EXPECT_TRUE(e.isInstanceOf(ctx, g_languageZeroDivision));
    }
    space.setErrorFactory(nullptr);
    EXPECT_EQ(space.getErrorFactory(), nullptr);
    try {
        (void) ctx->fromInteger(1)->modulo(ctx, ctx->fromInteger(0));
    } catch (const ProtoException& e) {
        EXPECT_FALSE(e.isInstanceOf(ctx, g_languageZeroDivision));
        EXPECT_TRUE(e.isInstanceOf(ctx, space.getErrorPrototype(ctx, ProtoErrorKind::DivisionByZero)));
    }
}

// Each kind protoCore raises today, from the public API.
TEST_F(ProtoExceptionTest, KindsRaisedByThePublicApi) {
    auto kindOf = [&](auto&& op) -> int {
        try {
            op();
        } catch (const ProtoException& e) {
            for (int k = 0; k < kProtoErrorKindCount; ++k)
                if (e.isInstanceOf(ctx, space.getErrorPrototype(ctx, static_cast<ProtoErrorKind>(k))))
                    return k;
            return -2;
        }
        return -1;
    };
    EXPECT_EQ(kindOf([&] { (void) ctx->fromString("x12", 10); }),
              static_cast<int>(ProtoErrorKind::InvalidArgument));
    EXPECT_EQ(kindOf([&] { (void) ctx->fromString("1", 40); }),
              static_cast<int>(ProtoErrorKind::InvalidArgument));
    EXPECT_EQ(kindOf([&] { (void) ctx->fromString("1180591620717411303424", 10)->asLong(ctx); }),
              static_cast<int>(ProtoErrorKind::Overflow));
    EXPECT_EQ(kindOf([&] { (void) ctx->fromInteger(3)->divide(ctx, ctx->fromInteger(0)); }),
              static_cast<int>(ProtoErrorKind::DivisionByZero));
    EXPECT_EQ(kindOf([&] { (void) ctx->fromInteger(3)->shiftLeft(ctx, -1); }),
              static_cast<int>(ProtoErrorKind::InvalidArgument));
}

// Too many positional arguments: raised in the caller's context before the
// new context registers, so the thread's current context is unchanged.
TEST_F(ProtoExceptionTest, ArityMismatchLeavesTheCurrentContextIntact) {
    const ProtoList* params = ctx->newList()->appendLast(ctx, ctx->fromUTF8String("a"));
    const ProtoObject* values[2] = {ctx->fromInteger(1), ctx->fromInteger(2)};
    const ProtoArguments two(values, 2);
    ProtoContext caller(&space, ctx, nullptr, nullptr, nullptr, nullptr);
    try {
        ProtoContext callee(&space, &caller, params, nullptr, &two, nullptr);
        FAIL() << "the call bound";
    } catch (const ProtoException& e) {
        EXPECT_TRUE(e.isInstanceOf(&caller, space.getErrorPrototype(&caller, ProtoErrorKind::ArityMismatch)));
    }
    // The callee never became the thread's current context.
    if (caller.thread) EXPECT_EQ(caller.thread->getCurrentContext(), &caller);
    // The caller is still usable: it allocates.
    const ProtoObject* o = caller.newObject(false)->setAttribute(&caller, sym(&caller, "x"), caller.fromInteger(9));
    EXPECT_EQ(o->getAttribute(&caller, sym(&caller, "x"))->asLong(&caller), 9);
}
