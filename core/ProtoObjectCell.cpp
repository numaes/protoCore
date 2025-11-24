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

    ProtoObjectCell::ProtoObjectCell(
        ProtoContext* context,
        const ParentLinkImplementation* parent,
        const ProtoSparseListImplementation* attributes,
        const unsigned long mutable_ref
    ) : Cell(context), parent(parent), mutable_ref(mutable_ref),
        attributes(attributes ? attributes : new(context) ProtoSparseListImplementation(context))
    {
    }

    ProtoObjectCell::~ProtoObjectCell() = default;


    // --- Interface Methods ---

    const ProtoObjectCell* ProtoObjectCell::addParent(
        ProtoContext* context, const ProtoObject* newParentToAdd) const
    {
        const auto* newParentLink = new(context) ParentLinkImplementation(
            context,
            this->parent,
            newParentToAdd
        );

        return new(context) ProtoObjectCell(
            context,
            newParentLink,
            this->attributes,
            this->mutable_ref
        );
    }

    const ProtoObject* ProtoObjectCell::asObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.objectCellImplementation = this;
        p.op.pointer_tag = POINTER_TAG_OBJECT;
        return p.oid.oid;
    }


    // --- Garbage Collector (GC) Methods ---

    void ProtoObjectCell::finalize(ProtoContext* context) const override
    {
    };

    // Corrected signature and implementation for const-correctness and proper GC interaction.
    void ProtoObjectCell::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const override
    {
        // 1. Report the direct reference to the parent link chain.
        if (this->parent)
        {
            method(context, self, this->parent);
        }

        // 2. Report the direct reference to the attributes list.
        if (this->attributes)
        {
            method(context, self, this->attributes);
        }
    }

} // namespace proto
