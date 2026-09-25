// CaseSymbols.cpp -- rule 4's runtime half.
//
// Rule 4 -- an attribute key must come from ProtoString::createSymbol, never
// from fromUTF8String.  The severity is NOT uniform, and this case tests the
// half that is a silent CORRECTNESS fault rather than the half that is merely
// slow.  Three different behaviours, all in core/ProtoObject.cpp:
//
//  1. setAttribute AUTO-INTERNS a STRING-tagged name before use.  Correct, at
//     the cost of a SymbolTable::intern per call.
//  2. getAttribute FALLS BACK to symbolTable->lookupByContent.  Correct, at the
//     cost of a content hash over the rope per read.
//  3. getOwnAttributeDirect does NEITHER: no STRING branch, no content
//     fallback.  It goes straight to the AttributeCache and the AVL probe, both
//     keyed on the RAW name pointer.
//
// So an uninterned key works through the general path and SILENTLY MISSES
// through the fast path -- and getOwnAttributeDirect is exactly the Phase-6
// LOAD_ATTR fast path that two runtimes in this family added.  That is the
// mechanism behind the repeated bugs in this class.
//
// The case makes the miss observable: write with the runtime's own key, then read
// back through the fast path with the SAME pointer the runtime would use.  The
// name is deliberately longer than INLINE_STRING_MAX_BYTES (6 bytes), because at
// six ASCII bytes or fewer protoCore embeds the string in the pointer word and
// two independently-built strings of the same short content ARE the same pointer
// -- so a short name matches by accident and a long one does not.  That accident
// is what makes this bug so hard to see: a 5-byte key works, a 7-byte key does
// not, and nothing errors.
#include "Cases.h"
#include "../headers/proto_internal.h"

#include <string>

namespace proto { namespace conformance {

CaseResult caseFastPathKeyHits(Host& host)
{
    const char* kId = "symbol.fast_path_key_hits";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 4, Status::Fail, "Host::mainContext() returned no usable context"};

    // Long enough that the inline-string accident cannot rescue it.
    const char* kName = "conformance_long_attribute_name";
    const unsigned long kNameLen = 31;

    const ProtoObject* keyObj = host.internAttributeKey(kName);
    if (!keyObj)
        return unavailable(kId, 4, "internAttributeKey",
                           "rule 4's runtime half needs the pointer this "
                           "runtime would actually use as an attribute key");

    if (kNameLen <= INLINE_STRING_MAX_BYTES)
        return {kId, 4, Status::NotApplicable,
                "the probe name is short enough to be an inline string, so a "
                "non-interned key would match by accident and this case could "
                "not fail.  This is a bug in the case, not in the runtime"};

    const ProtoString* key = reinterpret_cast<const ProtoString*>(keyObj);

    const ProtoObject* holder = ctx->newObject(/*mutableObject=*/false);
    const ProtoObject* value  = ctx->fromLong(0x5A5A5A);
    const ProtoObject* stored = holder->setAttribute(ctx, key, value);
    if (!stored || stored == PROTO_NONE)
        return {kId, 4, Status::Fail,
                "setAttribute rejected the key this runtime uses for attribute "
                "names"};

    // The general path: correct even for an uninterned key, because getAttribute
    // falls back to a content lookup.  If THIS misses, the key is not even
    // content-equal to what was written, which is a different and worse fault.
    const ProtoObject* viaGeneral = stored->getAttribute(ctx, key);

    // The fast path: no STRING branch, no content fallback, keyed on the raw
    // pointer.  This is the one that misses silently for an uninterned key.
    const ProtoObject* viaFastPath = stored->getOwnAttributeDirect(ctx, key);

    const std::string common =
        std::string("key=") + kName + " (" + std::to_string(kNameLen)
        + " bytes, INLINE_STRING_MAX_BYTES="
        + std::to_string(INLINE_STRING_MAX_BYTES)
        + "); getAttribute=" + (viaGeneral == PROTO_NONE ? "PROTO_NONE"
                                : (viaGeneral == nullptr ? "nullptr" : "value"))
        + "; getOwnAttributeDirect=" + (viaFastPath == nullptr ? "nullptr"
                                : (viaFastPath == PROTO_NONE ? "PROTO_NONE" : "value"));

    if (viaGeneral == nullptr || viaGeneral == PROTO_NONE)
        return {kId, 4, Status::Fail,
                "a value written under this runtime's own attribute key could "
                "not be read back through getAttribute, which falls back to a "
                "content lookup and should therefore succeed even for an "
                "uninterned key.  " + common};

    if (viaFastPath == nullptr)
        return {kId, 4, Status::Fail,
                "the value is readable through getAttribute but NOT through "
                "getOwnAttributeDirect, so the pointer this runtime uses as an "
                "attribute key is not the interned symbol protoCore stored the "
                "attribute under.  setAttribute auto-interned it and the write "
                "landed under the symbol; the fast path probes the AttributeCache "
                "and the AVL tree on the RAW pointer and misses.  Because "
                "nullptr is also getOwnAttributeDirect's value for 'absent', the "
                "miss is indistinguishable from a missing attribute and is "
                "therefore silent.  Use ProtoString::createSymbol for every key.  "
                + common};

    return {kId, 4, Status::Pass,
            "this runtime's attribute key is the interned symbol: a value "
            "written under it is readable through both the general path and the "
            "getOwnAttributeDirect fast path.  " + common};
}

}}  // namespace proto::conformance
