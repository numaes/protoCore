// 01_front_built_sequences.cpp -- tutorial 01 (docs/tutorials/linked-list/).
//
// ProtoLinkedList is the sequence that is built at the front, read at its
// head and dropped:
//   - several values returned by a native function, in one cell;
//   - a Lisp list: cons with prepend, first, rest, count;
//   - a walk with a cursor, which allocates nothing;
//   - asTuple when a canonical value (identity) is needed.
// Nothing is interned, so a list nobody references is garbage.
// Checks print ok / FAIL, so the example runs as a test.
#include "protoCore.h"

#include <cstdio>
#include <cstdlib>

using namespace proto;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// divmod(a, b) -> (quotient, remainder): two values in one cell.
const ProtoLinkedList* divmod(ProtoContext* ctx, long long a, long long b) {
    const ProtoObject* values[] = {ctx->fromInteger(a / b), ctx->fromInteger(a % b)};
    return ctx->newLinkedListFromArray(2, values);
}

// (reduce + list): a walk with a cursor allocates no cell.
long long sum(ProtoContext* ctx, const ProtoLinkedList* list) {
    long long total = 0;
    ProtoLinkedListCursor cursor(list);
    while (cursor.hasNext()) total += cursor.next()->asLong(ctx);
    return total;
}

}  // namespace

int main() {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    // Several values: one cell for up to four of them.
    const proto_ulong before = ctx->allocatedCellsCount;
    const ProtoLinkedList* qr = divmod(ctx, 17, 5);
    check(ctx->allocatedCellsCount - before == 1, "two returned values cost one cell");
    check(qr->getAt(ctx, 0)->asLong(ctx) == 3 && qr->getAt(ctx, 1)->asLong(ctx) == 2,
          "divmod(17, 5) is (3, 2)");

    // A Lisp list: (cons 1 (cons 2 (cons 3 ()))).
    const ProtoLinkedList* list = ctx->newLinkedList();
    for (int i = 3; i >= 1; --i) list = list->prepend(ctx, ctx->fromInteger(i));
    check(list->getSize(ctx) == 3 && list->getFirst(ctx)->asLong(ctx) == 1,
          "(count '(1 2 3)) is 3 and (first '(1 2 3)) is 1");
    check(list->getRest(ctx)->getFirst(ctx)->asLong(ctx) == 2, "(first (rest '(1 2 3))) is 2");
    check(ctx->newLinkedList()->getRest(ctx)->isEmpty(ctx), "(rest '()) is the empty list");

    // Versions share their tail: consing onto a list never changes it.
    const ProtoLinkedList* longer = list->prepend(ctx, ctx->fromInteger(0));
    check(list->getSize(ctx) == 3 && longer->getSize(ctx) == 4, "prepend returns a new version");
    check(longer->getRest(ctx)->isEqual(ctx, list), "(rest (cons 0 l)) equals l");

    // Walking allocates nothing.
    const proto_ulong beforeWalk = ctx->allocatedCellsCount;
    const long long total = sum(ctx, longer);
    check(total == 6 && ctx->allocatedCellsCount == beforeWalk, "a cursor walk allocates no cell");

    // A canonical value when identity matters, e.g. a key compared by
    // pointer: asTuple returns the interned tuple, the same for equal lists.
    // Interned tuples are perennial, so this is for keys and constants, not
    // for every list.
    const ProtoObject* again[] = {ctx->fromInteger(1), ctx->fromInteger(2), ctx->fromInteger(3)};
    const ProtoLinkedList* equal = ctx->newLinkedListFromArray(3, again);
    check(equal != list && equal->isEqual(ctx, list), "two equal lists are distinct values");
    check(equal->asTuple(ctx) == list->asTuple(ctx), "their asTuple is one interned tuple");

    std::printf("01_front_built_sequences: %s\n", g_failures ? "FAILED" : "PASS");
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
