/*
 * ProtoExternalPointer.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto
{
    // --- Constructor and Destructor ---

    ProtoExternalPointerImplementation::ProtoExternalPointerImplementation(
        ProtoContext* context,
        void* pointer
    ) : Cell(context), pointer(pointer)
    {
    }

    ProtoExternalPointerImplementation::~ProtoExternalPointerImplementation() = default;


    // --- Interface Methods ---

    void* ProtoExternalPointerImplementation::implGetPointer(ProtoContext* context) const
    {
        return this->pointer;
    }

    const ProtoObject* ProtoExternalPointerImplementation::implAsObject(ProtoContext* context) const override
    {
        ProtoObjectPointer p{};
        p.externalPointerImplementation = this;
        p.op.pointer_tag = POINTER_TAG_EXTERNAL_POINTER;
        return p.oid.oid;
    }

    // --- Garbage Collector (GC) Methods ---

    // Corrected signature for const-correctness.
    void ProtoExternalPointerImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const override
    {
        // This method is intentionally left empty.
        // A ProtoExternalPointer contains an opaque pointer (void*) that is not
        // managed by the Proto garbage collector.
    }

    void ProtoExternalPointerImplementation::finalize(ProtoContext* context) const override
    {
        // This method is intentionally left empty.
    };

    unsigned long ProtoExternalPointerImplementation::getHash(ProtoContext* context) const override
    {
        ProtoObjectPointer p{};
        p.externalPointerImplementation = this;
        return p.asHash.hash;
    }

} // namespace proto
