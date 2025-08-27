/*
* parent_link.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto
{
    // Using a member initialization list is more idiomatic and efficient in C++.
    ParentLinkImplementation::ParentLinkImplementation(
        ProtoContext* context,
        ParentLinkImplementation* parent,
        ProtoObjectCellImplementation* object
    ) : Cell(context), parent(parent), object(object)
    {
        // The constructor body can now be empty.
    };

    // The destructor does not need to perform any action.
    ParentLinkImplementation::~ParentLinkImplementation()
    {
    }

    void ParentLinkImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    )
    {
        // For the garbage collector, it is crucial to process all references to other Cells.

        // 1. Process the link to the previous parent in the chain.
        if (this->parent)
        {
            method(context, self, this->parent);
        }

        // 2. CRITICAL FIX: Process the object (ProtoObjectCell) that this link represents.
        // The previous version did not process this reference, which would cause the object
        // to be incorrectly collected by the GC.
        if (this->object)
        {
            method(context, self, reinterpret_cast<Cell*>(this->object));
        }

        // NOTE: The call to 'method(context, self, this)' was removed.
        // The garbage collector is already processing 'this' when it calls this method.
        // Passing it to itself again would cause an infinite loop during collection.
    }

    // The finalize method must be implemented as it is pure virtual in the base class.
    void ParentLinkImplementation::finalize(ProtoContext* context)
    {
        // This method is intentionally left empty because ParentLinkImplementation
        // does not acquire resources that require explicit cleanup.
    }

    ProtoObject* ParentLinkImplementation::asObject(ProtoContext* context)
    {
        return this->object->implAsObject(context);
    }

    unsigned long ParentLinkImplementation::getHash(ProtoContext* context)
    {
        // We should get the hash of the object that this link represents,
        // not the hash of the link itself.
        if (this->object)
        {
            return this->object->getHash(context);
        }
        // Return 0 or some other default value if there is no object.
        return 0;
    }
};
