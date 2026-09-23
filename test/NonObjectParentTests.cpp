// NonObjectParentTests.cpp — a non-object value stored as a "parent".
//
// addParent only rejects an EMBEDDED value (`ProtoObject::isCellPointer`
// is false for those — SmallInteger, boolean, PROTO_NONE, an inline
// string, ...); any OTHER cell-pointer type (a heap-allocated, non-inline
// ProtoString, for instance) is accepted as `newParent` and stored
// directly as a chain entry. setParents' own flattening (step 1 of
// flattenParentsOrder) has no such guard at all -- it accepts literally
// anything, including an embedded value like a SmallInteger.
//
// getAttribute and hasAttribute already handle a non-object chain entry
// correctly: their chain-navigation loop redirects to the entry's OWN
// prototype (a single hop) whenever the tag check fails, exactly as it
// does for a non-object RECEIVER. isInstanceOf/hasParent never dereference
// a chain entry as a ProtoObjectCell at all (pure pointer comparison), so
// they were never at risk either way.
//
// getAttributes()'s rewrite (this branch) was not: it called
// `toImpl<const ProtoObjectCell>(ancestor)` on every chain entry with NO
// tag check, so a non-object entry (whose tagged pointer bits do not
// address a ProtoObjectCell-shaped Cell) crashed the process on
// dereference. This file pins the fix -- the chosen policy is to mirror
// getAttribute exactly: a non-object chain entry contributes its OWN
// prototype's OWN attributes (one hop), not a crash and not silently
// nothing.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

class NonObjectParentTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space = nullptr;
    proto::ProtoContext* context = nullptr;

    void SetUp() override {
        space = new proto::ProtoSpace();
        context = space->rootContext;
    }
    void TearDown() override {
        delete space;
    }

    const ProtoString* sym(const char* s) { return ProtoString::createSymbol(context, s); }

    // Long enough to force a heap ProtoString (POINTER_TAG_STRING), not
    // an inline embedded string.
    const ProtoObject* heapString() {
        return context->fromUTF8String("a string long enough to force a heap allocation, not inline");
    }
};

// --- addParent accepts a heap string as a "parent" (pre-existing, --------
// --- unchanged policy: only an EMBEDDED value is rejected) ---------------

TEST_F(NonObjectParentTest, AddParentAcceptsAHeapStringParent) {
    const ProtoObject* d = context->newObject(false);
    const ProtoObject* str = heapString();
    d = d->addParent(context, str);

    EXPECT_EQ(d->hasParent(context, str), 1)
        << "addParent accepts any non-embedded-value cell pointer, including a heap string";
}

// --- setParents accepts even an embedded value (SmallInteger) ------------

TEST_F(NonObjectParentTest, SetParentsAcceptsASmallIntegerParent) {
    const ProtoObject* d = context->newObject(false);
    const ProtoObject* smallInt = context->fromInteger(42);
    const ProtoList* plist = context->newList()->appendLast(context, smallInt);
    d = d->setParents(context, plist);

    EXPECT_EQ(d->hasParent(context, smallInt), 1)
        << "flattenParentsOrder's step 1 has no tag filter";
}

// --- The fix: none of the four lookup methods crash on either shape ------

TEST_F(NonObjectParentTest, GetAttributesWithHeapStringParentDoesNotCrashAndFindsPrototypeAttribute) {
    const ProtoString* onStringProto = sym("onStringProto");
    // stringPrototype is IMMUTABLE: setAttribute returns a NEW handle
    // that must be re-published, or the write is invisible.
    context->space->stringPrototype = const_cast<ProtoObject*>(
        context->space->stringPrototype->setAttribute(context, onStringProto, context->fromInteger(1)));

    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, heapString());

    const ProtoSparseList* attrs = d->getAttributes(context);
    EXPECT_EQ(attrs->getAt(context, reinterpret_cast<uintptr_t>(onStringProto)), context->fromInteger(1))
        << "a non-object chain entry contributes its own prototype's own attributes";
}

TEST_F(NonObjectParentTest, GetAttributesWithSmallIntegerParentDoesNotCrashAndFindsPrototypeAttribute) {
    const ProtoString* onIntProto = sym("onIntProto");
    context->space->objectPrototype->setAttribute(context, onIntProto, context->fromInteger(2));
    // smallIntegerPrototype IS objectPrototype in this build (see
    // ProtoSpace.cpp), so setting it on objectPrototype is sufficient.

    const ProtoObject* d = context->newObject(false);
    const ProtoList* plist = context->newList()->appendLast(context, context->fromInteger(42));
    d = d->setParents(context, plist);

    const ProtoSparseList* attrs = d->getAttributes(context);
    EXPECT_EQ(attrs->getAt(context, reinterpret_cast<uintptr_t>(onIntProto)), context->fromInteger(2));
}

TEST_F(NonObjectParentTest, GetAttributeWithNonObjectParentDoesNotCrash) {
    const ProtoString* onStringProto = sym("onStringProto2");
    context->space->stringPrototype = const_cast<ProtoObject*>(
        context->space->stringPrototype->setAttribute(context, onStringProto, context->fromInteger(3)));

    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, heapString());

    EXPECT_EQ(d->getAttribute(context, onStringProto), context->fromInteger(3));
}

TEST_F(NonObjectParentTest, HasAttributeWithNonObjectParentDoesNotCrash) {
    const ProtoString* onStringProto = sym("onStringProto3");
    context->space->stringPrototype = const_cast<ProtoObject*>(
        context->space->stringPrototype->setAttribute(context, onStringProto, context->fromInteger(4)));

    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, heapString());

    EXPECT_EQ(d->hasAttribute(context, onStringProto), PROTO_TRUE);
}

TEST_F(NonObjectParentTest, IsInstanceOfWithNonObjectParentDoesNotCrash) {
    const ProtoObject* str = heapString();
    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, str);

    // isInstanceOf never dereferences a chain entry as a ProtoObjectCell
    // (pure pointer comparison); confirm this holds for a non-object
    // entry too -- correctly false for an unrelated prototype, no crash.
    EXPECT_EQ(d->isInstanceOf(context, context->space->listPrototype), PROTO_NONE);
    EXPECT_EQ(d->isInstanceOf(context, str), PROTO_TRUE)
        << "the heap string itself IS a direct entry of d's own chain";
}

// A not-found lookup past a non-object entry must still terminate cleanly.
TEST_F(NonObjectParentTest, NotFoundLookupPastNonObjectParentTerminates) {
    const ProtoString* missing = sym("definitelyMissing");
    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, heapString());

    EXPECT_EQ(d->getAttribute(context, missing), PROTO_NONE);
    EXPECT_EQ(d->hasAttribute(context, missing), PROTO_FALSE);
    const ProtoSparseList* attrs = d->getAttributes(context);
    EXPECT_FALSE(attrs->has(context, reinterpret_cast<uintptr_t>(missing)));
}
