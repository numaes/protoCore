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

    // Corrected: Removed 'override'
    const ProtoObject* ProtoExternalPointerImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.externalPointerImplementation = this;
        p.op.pointer_tag = POINTER_TAG_EXTERNAL_POINTER;
        return p.oid.oid;
    }

    // --- Garbage Collector (GC) Methods ---

    // Corrected: Removed 'override'
    void ProtoExternalPointerImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const
    {
        // Intentionally empty.
    }

    // Corrected: Removed 'override'
    void ProtoExternalPointerImplementation::finalize(ProtoContext* context) const
    {
        // Intentionally empty.
    }

    // Corrected: Removed 'override'
    unsigned long ProtoExternalPointerImplementation::getHash(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.externalPointerImplementation = this;
        return p.asHash.hash;
    }

} // namespace protoCore
