/*
* Cell.cpp
 *
 *  Created on: 2020-05-01
 *      Author: gamarino
 */


#include "../headers/proto_internal.h"

namespace proto
{
    Cell::Cell(ProtoContext* context)
    {
        // Each newly created Cell is immediately registered with the current context
        // for memory management and garbage collection tracking.
        context->addCell2Context(this);
    };

    // It is good practice to use '= default' for simple destructors in modern C++.
    Cell::~Cell() = default;

    unsigned long Cell::getHash(ProtoContext* context)
    {
        // The hash of a Cell is derived directly from its memory address.
        // This provides a fast and unique identifier for the object.
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;

        return p.asHash.hash;
    }

    // Base implementation for finalization.
    // Derived classes should override this method if they need to perform
    // any cleanup before being reclaimed by the garbage collector.
    void Cell::finalize(ProtoContext* context)
    {
        // Does nothing in the base class.
    };

    ProtoObject* Cell::asObject(ProtoContext* context)
    {
        return reinterpret_cast<ProtoObject*>(this);
    }

    // Overloads the 'new' operator to use the context's memory allocator.
    void* Cell::operator new(unsigned long size, ProtoContext* context)
    {
        return context->allocCell();
    };

    // Base implementation for the garbage collector's reference traversal.
    // Derived classes MUST override this method to call the 'method'
    // on every ProtoObject* or Cell* they contain, allowing the GC to mark reachable objects.
    void Cell::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    )
    {
        // Does nothing in the base class, as it contains no references to other objects.
    };
};
