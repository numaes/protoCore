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

    // The member initialization list is used, which is more efficient and idiomatic in C++.
    ProtoExternalPointerImplementation::ProtoExternalPointerImplementation(
        ProtoContext* context,
        void* pointer
    ) : Cell(context), pointer(pointer)
    {
        // The constructor body can now be empty.
    }

    // For empty destructors, using '= default' is the recommended practice.
    ProtoExternalPointerImplementation::~ProtoExternalPointerImplementation() {}


    // --- Interface Methods ---

    void* ProtoExternalPointerImplementation::implGetPointer(ProtoContext* context)
    {
        return this->pointer;
    }

    ProtoObject* ProtoExternalPointerImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;
        p.op.pointer_tag = POINTER_TAG_EXTERNAL_POINTER;
        return p.oid.oid;
    }

    // --- Garbage Collector (GC) Methods ---

    void ProtoExternalPointerImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    )
    {
        // This method is intentionally left empty.
        // A ProtoExternalPointer contains an opaque pointer (void*) that is not
        // managed by the Proto garbage collector. Therefore, there are no
        // references to other 'Cells' that need to be processed.
    }

    // An empty finalizer can also be declared as 'default'.
    void ProtoExternalPointerImplementation::finalize(ProtoContext* context)
    {
    };

    unsigned long ProtoExternalPointerImplementation::getHash(ProtoContext* context)
    {
        // The hash of a Cell is derived directly from its memory address.
        // This provides a fast and unique identifier for the object.
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;

        return p.asHash.hash;
    }
} // namespace proto
