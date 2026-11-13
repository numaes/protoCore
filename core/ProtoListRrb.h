// ProtoListRrb.h — the RRB-tree engine behind ProtoList's RRB form.
// Private to protoCore (core/ProtoList.cpp, core/ProtoContext.cpp, tests).
// Design and measurements: docs/LIST_RRB_DESIGN.md.
//
// Every function takes and returns RRB roots of any size >= 1; nullptr stands
// for the empty list.  The public ProtoList methods turn results of at most
// MAX_INLINE elements into the inline form.  Callers hold a
// ProtoContext::CriticalSection while they build (half-built paths live in C++
// locals until the root is returned).
#pragma once

#include "../headers/proto_internal.h"

namespace proto {
namespace rrb {

using Root = ProtoListRrbImplementation;
using Leaf = ProtoListRrbLeaf;

constexpr unsigned M = ProtoListRrbLeaf::WIDTH;

// Lists may hold up to this many elements (48-bit size tables).
constexpr proto_ulong kMaxSize = (static_cast<proto_ulong>(1) << 48) - 1;

// The root behind a tag-2 list handle, or nullptr for the AVL form.
const Root* rootOf(const ProtoList* list);

const Root* fromArray(ProtoContext* context, const ProtoObject* const* items, proto_ulong n);

const ProtoObject* getAt(const Root* r, proto_ulong i);
// The leaf (or the tail) holding element i, and the index range it covers.
const Leaf* locate(const Root* r, proto_ulong i, proto_ulong& start, proto_ulong& end);
// Calls fn(data, element) in order; stops early when fn returns true.
// Returns true when it stopped early.
bool forEach(const Root* r, bool (*fn)(void*, const ProtoObject*), void* data);
proto_ulong toBuffer(const Root* r, const ProtoObject** out);

const Root* setAt(ProtoContext* context, const Root* r, proto_ulong i, const ProtoObject* v);
const Root* appendLast(ProtoContext* context, const Root* r, const ProtoObject* v);
const Root* removeLast(ProtoContext* context, const Root* r);                 // nullptr when it empties
const Root* insertAt(ProtoContext* context, const Root* r, proto_ulong i, const ProtoObject* v);
const Root* removeAt(ProtoContext* context, const Root* r, proto_ulong i);    // nullptr when it empties
const Root* concat(ProtoContext* context, const Root* a, const Root* b);
const Root* take(ProtoContext* context, const Root* r, proto_ulong k);        // the first k elements
const Root* drop(ProtoContext* context, const Root* r, proto_ulong k);        // without the first k

}  // namespace rrb
}  // namespace proto
