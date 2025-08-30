/*
 * proto_object.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto
{
    // --- Constructor and Destructor ---

    // Modernized constructor with member initialization list.
    // ADJUSTED: The template type from 'ProtoSparseListImplementation' was removed.
    ProtoObjectCell::ProtoObjectCell(
        ProtoContext* context,
        ParentLinkImplementation* parent,
        ProtoSparseListImplementation* attributes,
        const unsigned long mutable_ref
    ) : Cell(context), parent(parent), mutable_ref(mutable_ref),
        attributes(attributes ? attributes : new(context) ProtoSparseListImplementation(context))
    {
        // The constructor body can now be empty.
    }

    // For empty destructors, using '= default' is the recommended practice.
    ProtoObjectCell::~ProtoObjectCell() = default;


    // --- Interface Methods ---

    ProtoObjectCell* ProtoObjectCell::addParent(
        ProtoContext* context, ProtoObject* newParentToAdd) const
    {
        // Creates a new link in the inheritance chain.
        auto* newParentLink = new(context) ParentLinkImplementation(
            context,
            this->parent, // The parent of the new link is our current parent.
            newParentToAdd // The object of the new link is the new parent.
        );

        // Returns a new ProtoObjectCell that is a copy of the current one,
        // but with the extended inheritance chain.
        return new(context) ProtoObjectCell(
            context,
            newParentLink,
            this->attributes,
            this->mutable_ref // The other properties are preserved.
        );
    }

    ProtoObject* ProtoObjectCell::asObject(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.objectCellImplementation = this;
        p.op.pointer_tag = POINTER_TAG_OBJECT;
        return p.oid.oid;
    }


    // --- Garbage Collector (GC) Methods ---

    // An empty finalizer can also be declared as 'default'.
    void ProtoObjectCell::finalize(ProtoContext* context)
    {
    };

    // Informs the GC about all internal references so they can be tracked.
    void ProtoObjectCell::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    )
    {
        // 1. Process the reference to the parent chain.
        if (this->parent)
        {
            method(context, self, this->parent);
        }

        // 2. Process the reference to the attributes list.
        if (this->attributes)
        {
            method(context, self, this->attributes);
        }
    }

    long unsigned ProtoObjectCell::getHash(ProtoContext* context)
    {
        // The hash of a Cell is derived directly from its memory address.
        // This provides a fast and unique identifier for the object.
        ProtoObjectPointer p{};
        p.objectCellImplementation = this;

        return p.asHash.hash;
    }

    // ------------------- ProtoObjectCell -------------------

    ProtoObject* ProtoObjectCell::asObject(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.oid.oid = toImpl<ProtoObjectCell>(this)->implAsObject(context);
        p.op.pointer_tag = POINTER_TAG_OBJECT;
        return p.oid.oid;
    }


} // namespace proto
