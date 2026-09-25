// GlobalInterningTests.cpp — P3: interning is process-global.
//
// Phase P3 makes ProtoString::createSymbol return one canonical pointer per
// spelling PER PROCESS, not per ProtoSpace.  Before P3 the guarantee was
// half-global with silent partial success: protoCore embeds a short ASCII
// string in the pointer word (INLINE_STRING_MAX_BYTES == 6), so a 5-byte name
// matched across spaces BY ACCIDENT while a 7-byte name missed with no error at
// all — getAttribute simply returned PROTO_NONE, which is also a value.
//
// Measured 2026-09-24 during protoST Track Y; the trap is recorded verbatim in
// protoST/src/modules/STModuleProvider.cpp.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <cstdio>
#include <string>

using namespace proto;

namespace {

// A space plus a context in it, parented to its root context.
struct Space {
    ProtoSpace   space;
    ProtoContext ctx{&space, space.rootContext, nullptr, nullptr, nullptr, nullptr};
};

}  // namespace

// The 5-byte case: inside INLINE_STRING_MAX_BYTES, so the pointer word carries
// the bytes and the two spaces agree WITHOUT any table.  This case passes both
// before and after P3, and it is in the suite to document the accident that made
// the 7-byte failure so hard to see.
TEST(GlobalInterning, ShortNameMatchedAcrossSpacesEvenBeforeP3) {
    Space a, b;
    const ProtoString* sa = ProtoString::createSymbol(&a.ctx, "value");   // 5 bytes
    const ProtoString* sb = ProtoString::createSymbol(&b.ctx, "value");
    ASSERT_NE(sa, nullptr);
    EXPECT_EQ(sa, sb) << "a name within INLINE_STRING_MAX_BYTES is embedded in "
                         "the pointer word and never reaches a symbol table";
}

// The 7-byte case: beyond INLINE_STRING_MAX_BYTES, so the name is a real
// interned cell.  BEFORE P3 THIS TEST FAILS.  That failure is the bug.
TEST(GlobalInterning, LongNameIsOnePointerAcrossSpaces) {
    Space a, b;
    const ProtoString* sa = ProtoString::createSymbol(&a.ctx, "Counter");  // 7 bytes
    const ProtoString* sb = ProtoString::createSymbol(&b.ctx, "Counter");
    ASSERT_NE(sa, nullptr);
    ASSERT_NE(sb, nullptr);
    EXPECT_EQ(sa, sb) << "interning must be process-global (P3)";
}

// The same failure as the family actually experiences it: a binding written in
// one space is unreadable from another, and the read reports ABSENCE rather than
// an error.  BEFORE P3 THIS TEST FAILS for "Counter" and would pass for "value".
TEST(GlobalInterning, AttributeWrittenInOneSpaceIsReadableFromAnother) {
    Space a, b;
    const ProtoString* keyA = ProtoString::createSymbol(&a.ctx, "Counter");
    const ProtoObject* holder =
        a.ctx.newObject(false)->setAttribute(&a.ctx, keyA, a.ctx.fromInteger(42));
    ASSERT_NE(holder, nullptr);

    const ProtoString* keyB = ProtoString::createSymbol(&b.ctx, "Counter");
    const ProtoObject* got = holder->getAttribute(&b.ctx, keyB);
    ASSERT_NE(got, nullptr);
    EXPECT_NE(got, PROTO_NONE)
        << "the read reported absence, not an error: this is the silent failure P3 closes";
    EXPECT_EQ(got->asLong(&b.ctx), 42);
}
