/*
 * ProtoHashedCollection.cpp — the shared hashed-collection helper over
 * ProtoSparseListObject (PSLO-SPEC §4).  Language callbacks (hash, equals)
 * always run BEFORE any GC critical section is entered: they may allocate,
 * run user code and reach a safepoint.  Only the construction of the new
 * version runs inside the critical section.
 *
 * D3 (PSLO-SPEC §7): every entry point ignores a nullptr key silently, before
 * any language callback runs.  A nullptr value in hashedPut removes the key,
 * as ProtoSparseListObject::setAt does.
 */

#include "../headers/proto_internal.h"

namespace proto
{
    namespace {
        constexpr unsigned long kSlotHashMask = (1UL << 54) - 1;

        // An embedded SmallInteger word carrying the hash's low 54 bits.
        // Built directly (never through fromInteger, which would allocate a
        // LargeInteger cell for values >= 2^53).
        const ProtoObject* hashSlotKey(unsigned long hash) {
            ProtoObjectPointer p{};
            p.op.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
            p.op.embedded_type = EMBEDDED_TYPE_SMALLINT;
            p.op.value = hash & kSlotHashMask;
            return p.oid;
        }

        bool isSmallIntegerWord(const ProtoObject* o) {
            ProtoObjectPointer p{};
            p.oid = o;
            return p.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && p.op.embedded_type == EMBEDDED_TYPE_SMALLINT;
        }

        bool usesIdentitySlot(ProtoContext* context, const KeySemantics& semantics, const ProtoObject* key) {
            return !isSmallIntegerWord(key) && semantics.isIdentityKey(context, key);
        }

        // Index of the key of the pair equal to `key` in a flat bucket, or -1.
        int findInBucket(ProtoContext* context, const ProtoList* bucket,
                         const KeySemantics& semantics, const ProtoObject* key) {
            const int n = static_cast<int>(bucket->getSize(context));
            for (int i = 0; i + 1 < n; i += 2)
                if (semantics.equals(context, bucket->getAt(context, i), key)) return i;
            return -1;
        }
    }

    const ProtoSparseListObject* hashedPut(ProtoContext* context, const ProtoSparseListObject* map,
                                           const KeySemantics& semantics, const ProtoObject* key, const ProtoObject* value) {
        if (!key) return map;
        if (!value) return hashedRemove(context, map, semantics, key);
        if (usesIdentitySlot(context, semantics, key)) return map->setAt(context, key, value);

        const ProtoObject* slot = hashSlotKey(semantics.hash(context, key));
        const ProtoObject* existing = map->getAt(context, slot);
        const ProtoList* bucket = existing ? existing->asList(context) : nullptr;
        const int at = bucket ? findInBucket(context, bucket, semantics, key) : -1;
        if (bucket && at >= 0 && bucket->getAt(context, at + 1) == value) return map;

        ProtoContext::CriticalSection cs(context);
        const ProtoList* newBucket;
        if (!bucket) {
            const ProtoObject* pair[2] = {key, value};
            newBucket = context->newList(2, pair);
        } else if (at >= 0) {
            newBucket = bucket->setAt(context, at + 1, value);       // keep the stored key (D2c)
        } else {
            newBucket = bucket->appendLast(context, key)->appendLast(context, value);
        }
        return map->setAt(context, slot, newBucket->asObject(context));
    }

    const ProtoObject* hashedGet(ProtoContext* context, const ProtoSparseListObject* map,
                                 const KeySemantics& semantics, const ProtoObject* key) {
        if (!key) return nullptr;
        if (usesIdentitySlot(context, semantics, key)) return map->getAt(context, key);
        const ProtoObject* existing = map->getAt(context, hashSlotKey(semantics.hash(context, key)));
        if (!existing) return nullptr;
        const ProtoList* bucket = existing->asList(context);
        const int at = findInBucket(context, bucket, semantics, key);
        return at >= 0 ? bucket->getAt(context, at + 1) : nullptr;
    }

    const ProtoSparseListObject* hashedRemove(ProtoContext* context, const ProtoSparseListObject* map,
                                              const KeySemantics& semantics, const ProtoObject* key) {
        if (!key) return map;
        if (usesIdentitySlot(context, semantics, key)) return map->removeAt(context, key);
        const ProtoObject* slot = hashSlotKey(semantics.hash(context, key));
        const ProtoObject* existing = map->getAt(context, slot);
        if (!existing) return map;
        const ProtoList* bucket = existing->asList(context);
        const int at = findInBucket(context, bucket, semantics, key);
        if (at < 0) return map;

        ProtoContext::CriticalSection cs(context);
        if (bucket->getSize(context) == 2) return map->removeAt(context, slot);
        const ProtoList* newBucket = bucket->removeAt(context, at)->removeAt(context, at);
        return map->setAt(context, slot, newBucket->asObject(context));
    }

    namespace {
        struct ForEachState {
            void* self;
            void (*fn)(ProtoContext*, void*, const ProtoObject*, const ProtoObject*);
        };

        void visitSlot(ProtoContext* context, void* raw, const ProtoObject* slotKey, const ProtoObject* slotValue) {
            auto* state = static_cast<ForEachState*>(raw);
            if (!isSmallIntegerWord(slotKey)) {
                state->fn(context, state->self, slotKey, slotValue);
                return;
            }
            const ProtoList* bucket = slotValue->asList(context);
            const int n = static_cast<int>(bucket->getSize(context));
            for (int i = 0; i + 1 < n; i += 2)
                state->fn(context, state->self, bucket->getAt(context, i), bucket->getAt(context, i + 1));
        }
    }

    void hashedForEach(ProtoContext* context, const ProtoSparseListObject* map, void* self,
                       void (*fn)(ProtoContext*, void*, const ProtoObject*, const ProtoObject*)) {
        if (!map || !fn) return;
        ForEachState state{self, fn};
        map->processElements(context, &state, visitSlot);
    }
}
