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

    // Corrected: Removed 'override' from the definition.
    void ParentLinkImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const
    {
        if (this->parent)
        {
            method(context, self, this->parent);
        }

        if (this->object && this->object->isCell(context))
        {
            method(context, self, this->object->asCell(context));
        }
    }

    // Corrected: Removed 'override' from the definition.
    void ParentLinkImplementation::finalize(ProtoContext* context) const
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
