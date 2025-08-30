/*
* Cell.cpp
 *
 *  Created on: 2020-05-01
 *      Author: gamarino
 */


#include "../headers/proto_internal.h"

namespace proto
{
    Cell::Cell(ProtoContext* context, Cell* next)
        : next(next)
    {
        // Each newly created Cell is immediately registered with the current context
        // for memory management and garbage collection tracking.
        context->addCell2Context(this);
    };

    unsigned long Cell::getHash(ProtoContext* context) const
    {
        // The hash of a Cell is derived directly from its memory address.
        // This provides a fast and unique identifier for the object.
        // The pointer is safely cast to an integer. To preserve the original
        // behavior of using a 60-bit hash, we apply a bitmask.
        // This approach avoids undefined behavior from union-based type punning.
        return reinterpret_cast<uintptr_t>(this) & ((1ULL << 60) - 1);
    }

    // Base implementation for finalization.
    // Derived classes should override this method if they need to perform
    // any cleanup before being reclaimed by the garbage collector.
    void Cell::finalize(ProtoContext* context)
    {
        // Does nothing in the base class.
    };

    ProtoObject* Cell::implAsObject(ProtoContext* context) const
    {
        // It should be implemented by subclasses
        return nullptr;
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
