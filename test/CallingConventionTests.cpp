// CallingConventionTests.cpp — one method type, two ways to call it (3.0.0).
//
// A ProtoMethod receives its arguments as ProtoArguments /
// ProtoKeywordArguments descriptors.  The same method is reached by the list
// form of ProtoObject::call (a ProtoList and a ProtoSparseList), by the array
// form (arrays the caller owns), and by the descriptor form (descriptors a
// method received and passes on).  These tests pin down:
//
//   * that the three forms reach the same method and that it reads the same
//     arguments, whatever the form;
//   * that the array form allocates nothing and hands the method the
//     caller's own array;
//   * the descriptors' read semantics, which mirror ProtoList and
//     ProtoSparseList (getAt outside the range answers PROTO_NONE);
//   * asList / asSparseList: the call's own list when there was one, a list
//     built from the array otherwise;
//   * ProtoContext's parameter binding from descriptors;
//   * nonMethodCallback and a thread's main method, which receive
//     descriptors too;
//   * that values held only in the caller context's automatic locals survive
//     collections that run during the call.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

using namespace proto;

namespace {

// What the probe method saw on its last call.
struct Seen {
    int calls = 0;
    const ProtoObject* self = nullptr;
    const ProtoObject* const* values = nullptr;
    std::vector<const ProtoObject*> positional;
    std::vector<ProtoKeywordArgument> keywords;
    const ProtoArguments* descriptor = nullptr;
    const ProtoKeywordArguments* keywordDescriptor = nullptr;
    const ProtoList* asList = nullptr;
    const ProtoSparseList* asSparseList = nullptr;
};
Seen gSeen;

const ProtoObject* probe(ProtoContext* context, const ProtoObject* self, const ParentLink*,
                         const ProtoArguments* positional, const ProtoKeywordArguments* keywords) {
    ++gSeen.calls;
    gSeen.self = self;
    gSeen.descriptor = positional;
    gSeen.keywordDescriptor = keywords;
    gSeen.positional.clear();
    gSeen.keywords.clear();
    gSeen.values = positional ? positional->values() : nullptr;
    if (positional) {
        for (unsigned i = 0; i < positional->getSize(context); ++i)
            gSeen.positional.push_back(positional->getAt(context, static_cast<int>(i)));
    }
    if (keywords) {
        for (unsigned i = 0; i < keywords->count(); ++i) gSeen.keywords.push_back(keywords->pairs()[i]);
    }
    return context->fromInteger(static_cast<long long>(gSeen.positional.size() * 100 + gSeen.keywords.size()));
}

// Passes its descriptors on, unchanged, to `probe` (the descriptor form).
const ProtoObject* forwarder(ProtoContext* context, const ProtoObject* self, const ParentLink*,
                             const ProtoArguments* positional, const ProtoKeywordArguments* keywords) {
    return self->call(context, nullptr, ProtoString::createSymbol(context, "probe"), self, *positional, keywords);
}

// Keeps its arguments as lists (what a method that retains them does).
const ProtoObject* keeper(ProtoContext* context, const ProtoObject*, const ParentLink*,
                          const ProtoArguments* positional, const ProtoKeywordArguments* keywords) {
    gSeen.asList = positional->asList(context);
    gSeen.asSparseList = keywords ? keywords->asSparseList(context) : nullptr;
    return gSeen.asList->asObject(context);
}

class CallingConvention : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext* root = nullptr;
    const ProtoString* probeName = nullptr;
    const ProtoObject* receiver = nullptr;

    void SetUp() override {
        root = space.rootContext;
        gSeen = Seen{};
        probeName = ProtoString::createSymbol(root, "probe");
        receiver = root->newObject(true);
        receiver->setAttribute(root, probeName, root->fromMethod(nullptr, &probe));
        receiver->setAttribute(root, ProtoString::createSymbol(root, "forward"),
                               root->fromMethod(nullptr, &forwarder));
        receiver->setAttribute(root, ProtoString::createSymbol(root, "keep"),
                               root->fromMethod(nullptr, &keeper));
    }
};

} // namespace

TEST_F(CallingConvention, ArrayFormHandsTheMethodTheCallersArrayAndAllocatesNothing) {
    const ProtoObject* args[3] = {root->fromInteger(1), root->fromInteger(2), root->fromInteger(3)};
    const ProtoString* k = ProtoString::createSymbol(root, "k");
    const ProtoKeywordArgument kw[1] = {{k, root->fromInteger(9)}};

    ProtoContext caller(&space, root);
    const proto_ulong before = caller.allocatedCellsCount;
    const ProtoObject* r = receiver->call(&caller, nullptr, probeName, receiver, args, 3, kw, 1);
    const proto_ulong allocated = caller.allocatedCellsCount - before;

    EXPECT_EQ(allocated, 0u) << "no list, no sparse list, no copy; the result is an embedded integer";
    EXPECT_EQ(r->asLong(&caller), 301);
    EXPECT_EQ(gSeen.values, args) << "the method reads the caller's array itself";
    ASSERT_EQ(gSeen.positional.size(), 3u);
    EXPECT_EQ(gSeen.positional[2]->asLong(&caller), 3);
    ASSERT_EQ(gSeen.keywords.size(), 1u);
    EXPECT_EQ(gSeen.keywords[0].name, k);
    EXPECT_EQ(gSeen.self, receiver);
}

TEST_F(CallingConvention, ListFormReachesTheSameMethodWithTheSameArguments) {
    const ProtoString* k = ProtoString::createSymbol(root, "k");
    const ProtoList* list = root->newList()
        ->appendLast(root, root->fromInteger(1))
        ->appendLast(root, root->fromInteger(2));
    const ProtoSparseList* dict = root->newSparseList()
        ->setAt(root, reinterpret_cast<proto_ulong>(k), root->fromInteger(9));

    ProtoContext caller(&space, root);
    const proto_ulong before = caller.allocatedCellsCount;
    const ProtoObject* r = receiver->call(&caller, nullptr, probeName, receiver, list, dict);
    EXPECT_EQ(caller.allocatedCellsCount - before, 0u)
        << "a small list is read in place and the keywords are copied to the stack";

    EXPECT_EQ(r->asLong(root), 201);
    ASSERT_EQ(gSeen.positional.size(), 2u);
    EXPECT_EQ(gSeen.positional[0]->asLong(root), 1);
    EXPECT_EQ(gSeen.positional[1]->asLong(root), 2);
    ASSERT_EQ(gSeen.keywords.size(), 1u);
    EXPECT_EQ(gSeen.keywords[0].name, k);
    EXPECT_EQ(gSeen.keywords[0].value->asLong(root), 9);
}

TEST_F(CallingConvention, ListFormWithoutListsPassesAnEmptyDescriptorAndNoKeywords) {
    const ProtoObject* r = receiver->call(root, nullptr, probeName, receiver, nullptr, nullptr);
    EXPECT_EQ(r->asLong(root), 0);
    ASSERT_NE(gSeen.descriptor, nullptr) << "the positional descriptor is never null";
    EXPECT_EQ(gSeen.keywordDescriptor, nullptr);

    const ProtoObject* r2 = receiver->call(root, nullptr, probeName, receiver,
                                           static_cast<const ProtoObject* const*>(nullptr), 0u);
    EXPECT_EQ(r2->asLong(root), 0);
    ASSERT_NE(gSeen.descriptor, nullptr);
    EXPECT_EQ(gSeen.keywordDescriptor, nullptr);
}

TEST_F(CallingConvention, DescriptorFormPassesReceivedArgumentsOnWithoutCopying) {
    const ProtoObject* args[2] = {root->fromInteger(5), root->fromInteger(6)};
    const ProtoKeywordArgument kw[1] = {{ProtoString::createSymbol(root, "z"), root->fromInteger(7)}};
    const ProtoObject* r = receiver->call(root, nullptr, ProtoString::createSymbol(root, "forward"),
                                          receiver, args, 2, kw, 1);
    EXPECT_EQ(r->asLong(root), 201);
    EXPECT_EQ(gSeen.values, args) << "forwarded descriptors still read the first caller's array";
}

TEST_F(CallingConvention, ListFormCopiesMoreValuesThanFitInline) {
    const ProtoList* list = root->newList();
    const unsigned n = ProtoListArguments::kInline * 3 + 1;
    for (unsigned i = 0; i < n; ++i) list = list->appendLast(root, root->fromInteger(i));
    receiver->call(root, nullptr, probeName, receiver, list, nullptr);
    ASSERT_EQ(gSeen.positional.size(), n);
    for (unsigned i = 0; i < n; ++i) EXPECT_EQ(gSeen.positional[i]->asLong(root), static_cast<long long>(i));

    const ProtoSparseList* dict = root->newSparseList();
    const unsigned kn = ProtoSparseListKeywordArguments::kInline * 2 + 1;
    std::vector<const ProtoString*> names;
    for (unsigned i = 0; i < kn; ++i) {
        names.push_back(ProtoString::createSymbol(root, ("kw" + std::to_string(i)).c_str()));
        dict = dict->setAt(root, reinterpret_cast<proto_ulong>(names.back()), root->fromInteger(i));
    }
    receiver->call(root, nullptr, probeName, receiver, nullptr, dict);
    EXPECT_EQ(gSeen.keywords.size(), kn);
}

TEST(CallingConventionDescriptors, PositionalReadsMirrorProtoList) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoObject* values[3] = {c->fromInteger(10), nullptr, c->fromInteger(30)};
    const ProtoArguments a(values, 3);

    EXPECT_EQ(a.getSize(c), 3u);
    EXPECT_EQ(a.count(), 3u);
    EXPECT_EQ(a.getAt(c, 0)->asLong(c), 10);
    EXPECT_EQ(a.getAt(c, 1), PROTO_NONE) << "a null slot reads as PROTO_NONE, as in a ProtoList";
    EXPECT_EQ(a.getAt(c, 3), PROTO_NONE);
    EXPECT_EQ(a.getAt(c, -1), PROTO_NONE);
    EXPECT_EQ(a.getFirst(c)->asLong(c), 10);
    EXPECT_EQ(a.getLast(c)->asLong(c), 30);
    EXPECT_TRUE(a.has(c, c->fromInteger(30)));
    EXPECT_FALSE(a.has(c, c->fromInteger(20)));

    const ProtoArguments empty(nullptr, 5);
    EXPECT_EQ(empty.getSize(c), 0u) << "a null array has no values whatever the count";
    EXPECT_EQ(empty.getFirst(c), PROTO_NONE);
    EXPECT_EQ(empty.getLast(c), PROTO_NONE);
}

TEST(CallingConventionDescriptors, KeywordReadsMirrorProtoSparseList) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoString* a = ProtoString::createSymbol(c, "a");
    const ProtoString* b = ProtoString::createSymbol(c, "b");
    const ProtoKeywordArgument pairs[2] = {{a, c->fromInteger(1)}, {b, c->fromInteger(2)}};
    const ProtoKeywordArguments k(pairs, 2);

    EXPECT_EQ(k.getSize(c), 2u);
    EXPECT_TRUE(k.has(c, reinterpret_cast<proto_ulong>(b)));
    EXPECT_FALSE(k.has(c, reinterpret_cast<proto_ulong>(ProtoString::createSymbol(c, "q"))));
    EXPECT_EQ(k.getAt(c, reinterpret_cast<proto_ulong>(a))->asLong(c), 1);
    EXPECT_EQ(k.getAt(c, 12345u), PROTO_NONE);

    struct Collect {
        std::vector<proto_ulong> keys;
        static void add(ProtoContext*, void* self, proto_ulong key, const ProtoObject*) {
            static_cast<Collect*>(self)->keys.push_back(key);
        }
    } collect;
    k.processElements(c, &collect, &Collect::add);
    ASSERT_EQ(collect.keys.size(), 2u);
    EXPECT_EQ(collect.keys[0], reinterpret_cast<proto_ulong>(a)) << "array order";
    EXPECT_EQ(collect.keys[1], reinterpret_cast<proto_ulong>(b));
}

TEST_F(CallingConvention, AsListAnswersTheCallsListOrBuildsOne) {
    const ProtoString* keepName = ProtoString::createSymbol(root, "keep");
    const ProtoString* k = ProtoString::createSymbol(root, "k");

    const ProtoList* list = root->newList()->appendLast(root, root->fromInteger(4));
    const ProtoSparseList* dict = root->newSparseList()
        ->setAt(root, reinterpret_cast<proto_ulong>(k), root->fromInteger(8));
    receiver->call(root, nullptr, keepName, receiver, list, dict);
    EXPECT_EQ(gSeen.asList, list) << "the list form's own list, no copy";
    EXPECT_EQ(gSeen.asSparseList, dict);

    const ProtoObject* args[2] = {root->fromInteger(4), root->fromInteger(5)};
    const ProtoKeywordArgument kw[1] = {{k, root->fromInteger(8)}};
    receiver->call(root, nullptr, keepName, receiver, args, 2, kw, 1);
    ASSERT_NE(gSeen.asList, nullptr);
    ASSERT_EQ(gSeen.asList->getSize(root), 2u);
    EXPECT_EQ(gSeen.asList->getAt(root, 1)->asLong(root), 5);
    ASSERT_NE(gSeen.asSparseList, nullptr);
    EXPECT_EQ(gSeen.asSparseList->getAt(root, reinterpret_cast<proto_ulong>(k))->asLong(root), 8);
}

TEST_F(CallingConvention, InheritedMethodIsFoundByEveryForm) {
    const ProtoObject* child = receiver->newChild(root);
    const ProtoObject* args[1] = {root->fromInteger(1)};
    EXPECT_EQ(child->call(root, nullptr, probeName, child, args, 1)->asLong(root), 100);
    EXPECT_EQ(gSeen.self, child);
    const ProtoList* list = root->newList()->appendLast(root, root->fromInteger(1));
    EXPECT_EQ(child->call(root, nullptr, probeName, child, list)->asLong(root), 100);
}

TEST_F(CallingConvention, MissingMethodWithoutCallbackAnswersNone) {
    const ProtoString* missing = ProtoString::createSymbol(root, "missing");
    const ProtoObject* args[1] = {root->fromInteger(1)};
    EXPECT_EQ(receiver->call(root, nullptr, missing, receiver, args, 1), PROTO_NONE);
    EXPECT_EQ(receiver->call(root, nullptr, missing, receiver, nullptr, nullptr), PROTO_NONE);
    EXPECT_EQ(gSeen.calls, 0);
}

namespace {
int gNonMethodCalls = 0;
unsigned gNonMethodArgs = 0;
ProtoObject* nonMethod(ProtoContext*, const ParentLink*, const ProtoString*, const ProtoObject*,
                       const ProtoArguments* positional, const ProtoKeywordArguments* keywords) {
    ++gNonMethodCalls;
    gNonMethodArgs = positional->count() * 10 + (keywords ? keywords->count() : 0);
    return const_cast<ProtoObject*>(PROTO_TRUE);
}
}

TEST_F(CallingConvention, NonMethodCallbackReceivesDescriptorsInBothForms) {
    gNonMethodCalls = 0;
    space.nonMethodCallback = &nonMethod;
    const ProtoString* field = ProtoString::createSymbol(root, "field");
    receiver->setAttribute(root, field, root->fromInteger(3));

    const ProtoObject* args[2] = {root->fromInteger(1), root->fromInteger(2)};
    const ProtoKeywordArgument kw[1] = {{field, root->fromInteger(1)}};
    EXPECT_EQ(receiver->call(root, nullptr, field, receiver, args, 2, kw, 1), PROTO_TRUE);
    EXPECT_EQ(gNonMethodArgs, 21u);

    const ProtoList* list = root->newList()->appendLast(root, root->fromInteger(1));
    EXPECT_EQ(receiver->call(root, nullptr, field, receiver, list, nullptr), PROTO_TRUE);
    EXPECT_EQ(gNonMethodArgs, 10u);
    EXPECT_EQ(gNonMethodCalls, 2);
    space.nonMethodCallback = nullptr;
}

TEST(CallingConventionBinding, ContextBindsParametersFromArraysAndFromLists) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    const ProtoString* a = ProtoString::createSymbol(root, "a");
    const ProtoString* b = ProtoString::createSymbol(root, "b");
    const ProtoList* parameterNames = root->newList()
        ->appendLast(root, reinterpret_cast<const ProtoObject*>(a))
        ->appendLast(root, reinterpret_cast<const ProtoObject*>(b));

    const ProtoObject* values[1] = {root->fromInteger(1)};
    const ProtoKeywordArgument pairs[1] = {{b, root->fromInteger(2)}};
    const ProtoArguments positional(values, 1);
    const ProtoKeywordArguments keywords(pairs, 1);
    {
        ProtoContext f(&space, root, parameterNames, nullptr, &positional, &keywords);
        EXPECT_EQ(f.closureLocals->getAt(root, a->getHash(root))->asLong(root), 1);
        EXPECT_EQ(f.closureLocals->getAt(root, b->getHash(root))->asLong(root), 2)
            << "a keyword keyed by the interned name's pointer binds its parameter";
    }

    // The list form, keyed by content hash as the binding was before 3.0.0.
    const ProtoList* list = root->newList()->appendLast(root, root->fromInteger(1));
    const ProtoSparseList* dict = root->newSparseList()->setAt(root, b->getHash(root), root->fromInteger(2));
    const ProtoListArguments listPositional(root, list);
    const ProtoSparseListKeywordArguments listKeywords(root, dict);
    {
        ProtoContext f(&space, root, parameterNames, nullptr, &listPositional, &listKeywords);
        EXPECT_EQ(f.closureLocals->getAt(root, a->getHash(root))->asLong(root), 1);
        EXPECT_EQ(f.closureLocals->getAt(root, b->getHash(root))->asLong(root), 2);
    }

    const ProtoObject* tooMany[3] = {root->fromInteger(1), root->fromInteger(2), root->fromInteger(3)};
    const ProtoArguments three(tooMany, 3);
    EXPECT_THROW({ ProtoContext f(&space, root, parameterNames, nullptr, &three, nullptr); },
                 std::invalid_argument);
}

namespace {
std::atomic<long long> gThreadSum{0};
const ProtoObject* threadMain(ProtoContext* context, const ProtoObject*, const ParentLink*,
                              const ProtoArguments* positional, const ProtoKeywordArguments*) {
    long long sum = 0;
    for (unsigned i = 0; i < positional->count(); ++i) sum += positional->getAt(context, static_cast<int>(i))->asLong(context);
    gThreadSum = sum;
    return PROTO_NONE;
}
}

TEST(CallingConventionThreads, ThreadMainReceivesItsListAsADescriptor) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    gThreadSum = 0;
    const ProtoList* args = root->newList()
        ->appendLast(root, root->fromInteger(20))
        ->appendLast(root, root->fromInteger(22));
    const ProtoThread* t = space.newThread(root, ProtoString::createSymbol(root, "cc-thread"),
                                           &threadMain, args, nullptr);
    const_cast<ProtoThread*>(t)->join(root);
    EXPECT_EQ(gThreadSum.load(), 42);
}

namespace {
// Allocates enough garbage, in contexts of its own, to run collections while
// the caller's arguments are reachable only from the caller's automatic
// locals; then reads them.
const ProtoString* gValueKey = nullptr;
long long gReadBack = 0;
uint64_t gCyclesDuringCall = 0;
const ProtoObject* allocatingCallee(ProtoContext* context, const ProtoObject*, const ParentLink*,
                                    const ProtoArguments* positional, const ProtoKeywordArguments*) {
    ProtoSpace* space = context->space;
    const uint64_t start = space->getGCCycleCount();
    space->setHeapLimits(/*soft=*/0, /*hard=*/space->heapSize + 20000);
    for (int batch = 0; batch < 400 && space->getGCCycleCount() - start < 2; ++batch) {
        ProtoContext garbage(space, context);
        for (int i = 0; i < 20000; ++i) (void) garbage.newObject(false);
    }
    space->setHeapLimits(0, 0);
    gCyclesDuringCall = space->getGCCycleCount() - start;
    long long sum = 0;
    for (unsigned i = 0; i < positional->count(); ++i)
        sum += positional->values()[i]->getAttribute(context, gValueKey)->asLong(context);
    gReadBack = sum;
    return PROTO_NONE;
}
}

TEST(CallingConventionGC, ArgumentsRootedOnlyByCallerSlotsSurviveCollectionsDuringTheCall) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    gValueKey = ProtoString::createSymbol(root, "v");
    const ProtoString* name = ProtoString::createSymbol(root, "allocating");
    const ProtoObject* receiver = root->newObject(true);
    receiver->setAttribute(root, name, root->fromMethod(nullptr, &allocatingCallee));

    constexpr unsigned kArgs = 6;
    ProtoContext caller(&space, root, nullptr, nullptr, nullptr, nullptr, kArgs);
    long long expected = 0;
    for (unsigned i = 0; i < kArgs; ++i) {
        // Fresh objects, held by nothing but the caller's slot.
        caller.setAutomaticLocal(i, caller.newObject()->setAttribute(&caller, gValueKey,
                                                                      caller.fromInteger(1000 + i)));
        expected += 1000 + i;
    }
    receiver->call(&caller, nullptr, name, receiver, caller.getAutomaticLocals(), kArgs);
    EXPECT_GE(gCyclesDuringCall, 1u) << "the test must run a collection during the call";
    EXPECT_EQ(gReadBack, expected);
}
