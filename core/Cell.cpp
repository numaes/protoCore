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
        if (context) {
            context->addCell2Context(this);
        }
    };

    unsigned long Cell::getHash(ProtoContext* context) const
    {
        // The hash of a Cell is derived directly from its memory address.
        // This provides a fast and unique identifier for the object.
        return reinterpret_cast<uintptr_t>(this);
    }

    void Cell::finalize(ProtoContext* context) const
    {
        // Base implementation does nothing.
    };

    const ProtoObject* Cell::implAsObject(ProtoContext* context) const
    {
        // Base implementation returns nullptr. Subclasses must override this.
        return nullptr;
    }

    const Cell* Cell::asCell(ProtoContext* context) const
    {
        // A const method should return a const pointer to itself.
        return this;
    }

    void* Cell::operator new(unsigned long size, ProtoContext* context)
    {
        return context->allocCell();
    };

    // Corrected signature to match the declaration and expected const-correctness.
    // The callback now correctly accepts a const Cell*.
    void Cell::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const
    {
        // Base implementation does nothing as it holds no references.
    };
};
