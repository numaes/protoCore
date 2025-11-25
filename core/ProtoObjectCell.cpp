/*
 * ProtoObjectCell.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 *
 *  This file implements the internal representation of a standard Proto object.
 */

#include "../headers/proto_internal.h"

namespace proto
{
    /**
     * @class ProtoObjectCell
     * @brief The internal implementation of a standard, user-creatable object.
     *
     * This class is the concrete representation of a dynamic object in Proto.
     * It combines the two key aspects of the object model: its own attributes
     * and its link to a prototype chain for inheritance.
     */

    /**
     * @brief Constructs a new object cell.
     * @param context The current execution context.
     * @param parent A pointer to the `ParentLink` that forms the head of the prototype chain.
     * @param attributes A `ProtoSparseList` holding the object's own key-value attributes.
     * @param mutable_ref A non-zero ID if this object is a mutable reference, otherwise 0.
     */
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

    /**
     * @brief Creates a new object that inherits from the current one.
     * This is a key part of the immutable API. It returns a new `ProtoObjectCell`
     * whose prototype chain includes the `newParentToAdd`.
     * @param context The current execution context.
     * @param newParentToAdd The object to be added as the immediate parent of the new object.
     * @return A new `ProtoObjectCell` instance.
     */
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

    /**
     * @brief Converts this internal implementation object into its public API handle.
     * This method sets the correct type tag on a `ProtoObjectPointer` union to create
     * a valid `ProtoObject*` that can be returned to the user.
     */
    const ProtoObject* ProtoObjectCell::asObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.objectCellImplementation = this;
        p.op.pointer_tag = POINTER_TAG_OBJECT;
        return p.oid.oid;
    }

    /**
     * @brief Finalizer for the ProtoObjectCell.
     * This object holds no external resources, so the finalizer is empty.
     */
    void ProtoObjectCell::finalize(ProtoContext* context) const
    {
    };

    /**
     * @brief Informs the GC about the cells this object holds references to.
     * An object cell holds references to its parent link chain and its own
     * attribute list. Both must be reported to the GC to prevent them from
     * being prematurely collected.
     */
    void ProtoOffice_object.cppbjectCell::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const
    {
        // Report the head of the prototype chain.
        if (this->parent)
        {
            method(context, self, this->parent);
        }

        // Report the attribute list.
        if (this->attributes)
        {
            method(context, self, this->attributes);
        }
    }

} // namespace proto
