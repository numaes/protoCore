/*
* parent_link.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto
{
    ParentLinkImplementation::ParentLinkImplementation(
        ProtoContext* context,
        const ParentLinkImplementation* parent,
        const ProtoObject* object
    ) : Cell(context), parent(parent), object(object)
    {
    };

    ParentLinkImplementation::~ParentLinkImplementation() = default;

    // Corrected signature for const-correctness.
    void ParentLinkImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const override
    {
        // For the garbage collector, it is crucial to process all references to other Cells.

        // 1. Process the link to the previous parent in the chain.
        if (this->parent)
        {
            // 'parent' is already a 'const ParentLinkImplementation*', which is a const Cell*.
            method(context, self, this->parent);
        }

        // 2. Process the object (ProtoObjectCell) that this link represents.
        if (this->object && this->object->isCell(context))
        {
            // 'object' is a 'const ProtoObject*'. Its asCell() method returns a 'const Cell*'.
            method(context, self, this->object->asCell(context));
        }
    }

    void ParentLinkImplementation::finalize(ProtoContext* context) const override
    {
        // This method is intentionally left empty.
    }

    const ProtoObject* ParentLinkImplementation::getObject(ProtoContext* context) const
    {
        return this->object;
    };

    const ParentLinkImplementation* ParentLinkImplementation::getParent(ProtoContext* context) const
    {
        return this->parent;
    };
};
