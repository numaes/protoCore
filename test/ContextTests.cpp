#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h" // For internal class access
#include <stdexcept>

using namespace proto;

class ContextTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space;
    proto::ProtoContext* rootContext;

    void SetUp() override {
        space = new proto::ProtoSpace();
        rootContext = space->rootContext;
    }

    void TearDown() override {
        delete space;
    }
};

// Test 1: Correct binding of positional arguments to parameters
TEST_F(ContextTest, PositionalArgumentBinding) {
    // Simulating a function f(a, b)
    const ProtoList* paramNames = rootContext->newList()
        ->appendLast(rootContext, rootContext->fromUTF8String("a"))
        ->appendLast(rootContext, rootContext->fromUTF8String("b"));

    // Called with (10, "hello")
    const ProtoList* args = rootContext->newList()
        ->appendLast(rootContext, rootContext->fromLong(10))
        ->appendLast(rootContext, rootContext->fromUTF8String("hello"));

    ProtoContext funcContext(space, rootContext, paramNames, nullptr, args, nullptr);

    const ProtoObject* val_a = funcContext.closureLocals->getAt(rootContext, rootContext->fromUTF8String("a")->getHash(rootContext));
    const ProtoObject* val_b = funcContext.closureLocals->getAt(rootContext, rootContext->fromUTF8String("b")->getHash(rootContext));

    ASSERT_NE(val_a, PROTO_NONE);
    ASSERT_NE(val_b, PROTO_NONE);
    ASSERT_EQ(val_a->asLong(rootContext), 10);
    ASSERT_STREQ(val_b->asString(rootContext)->asUTF8(rootContext), "hello");
}

// Test 2: Allocation of automatic and closure local variables
TEST_F(ContextTest, LocalVariableAllocation) {
    // Simulating f(param1) with local variables x, y
    const ProtoList* paramNames = rootContext->newList()->appendLast(rootContext, rootContext->fromUTF8String("param1"));
    const ProtoList* localNames = rootContext->newList()
        ->appendLast(rootContext, rootContext->fromUTF8String("x"))
        ->appendLast(rootContext, rootContext->fromUTF8String("y"));
    const ProtoList* args = rootContext->newList()->appendLast(rootContext, rootContext->fromLong(123));

    ProtoContext funcContext(space, rootContext, paramNames, localNames, args, nullptr);

    // Check automatic locals
    ASSERT_EQ(funcContext.getAutomaticLocalsCount(), 2);
    ASSERT_NE(funcContext.getAutomaticLocals(), nullptr);
    // Ensure they are initialized to NONE
    ASSERT_EQ(funcContext.getAutomaticLocals()[0], PROTO_NONE);
    ASSERT_EQ(funcContext.getAutomaticLocals()[1], PROTO_NONE);

    // Check closure locals (parameters)
    ASSERT_EQ(funcContext.closureLocals->getSize(rootContext), 1);
    const ProtoObject* param_val = funcContext.closureLocals->getAt(rootContext, rootContext->fromUTF8String("param1")->getHash(rootContext));
    ASSERT_EQ(param_val->asLong(rootContext), 123);

    // Automatic locals should not be in the closure map
    ASSERT_EQ(funcContext.closureLocals->getAt(rootContext, rootContext->fromUTF8String("x")->getHash(rootContext)), PROTO_NONE);
}

// Test 3: Error handling for incorrect argument counts
TEST_F(ContextTest, ArgumentCountError) {
    // Simulating f(a) called with (1, 2)
    const ProtoList* paramNames = rootContext->newList()->appendLast(rootContext, rootContext->fromUTF8String("a"));
    const ProtoList* args = rootContext->newList()
        ->appendLast(rootContext, rootContext->fromLong(1))
        ->appendLast(rootContext, rootContext->fromLong(2));

    ASSERT_THROW({
        ProtoContext funcContext(space, rootContext, paramNames, nullptr, args, nullptr);
    }, std::invalid_argument);
}

// Test 4: The full lifecycle and return value promotion
TEST_F(ContextTest, LifecycleAndReturnValuePromotion) {
    // 1. Create a "parent" context (e.g., for function g)
    ProtoContext* g_context = new ProtoContext(space, rootContext, nullptr, nullptr, nullptr, nullptr);
    
    // 2. Create a "child" context (e.g., for function f, called by g)
    ProtoContext* f_context = new ProtoContext(space, g_context, nullptr, nullptr, nullptr, nullptr);

    // 3. In the child context, create a new heap-allocated object
    const ProtoList* new_list = f_context->newList()->appendLast(f_context, f_context->fromLong(100));
    
    // The new list is the first and only cell allocated in f_context
    ASSERT_EQ(f_context->lastAllocatedCell, new_list->asCell(f_context));
    ASSERT_EQ(g_context->lastAllocatedCell, nullptr); // Parent is untouched

    // 4. Set the return value for f_context
    f_context->returnValue = new_list;

    // 5. Destroy f_context, simulating the function return
    delete f_context;

    // 6. Assert the state of the parent context (g_context)
    // The parent's "young generation" should now contain exactly one cell.
    ASSERT_NE(g_context->lastAllocatedCell, nullptr);
    ASSERT_EQ(g_context->lastAllocatedCell->next, nullptr);

    // This cell must be a ReturnReference
    const Cell* ref_cell = g_context->lastAllocatedCell;
    // We use dynamic_cast for testing to verify the type.
    const ReturnReference* return_ref = dynamic_cast<const ReturnReference*>(ref_cell);
    ASSERT_NE(return_ref, nullptr);

    // And the ReturnReference must point to our original list.
    ASSERT_EQ(return_ref->returnValue, new_list->asCell(g_context));

    // Cleanup
    delete g_context;
}
