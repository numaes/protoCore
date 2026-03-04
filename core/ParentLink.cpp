/*
 * ParentLink.cpp
 *
 *  Created on: Aug 6, 2017
 *      Author: gamarino
 *
 *  This file implements the ParentLink, a core component of Proto's
 *  prototype-based inheritance mechanism.
 */

#include "../headers/proto_internal.h"

namespace proto
{
    /**
     * @class ParentLinkImplementation
     * @brief A node in a linked list representing an object's prototype chain.
     *
     * This class is fundamental to how Proto handles inheritance. Each `ProtoObject`
     * points to a `ParentLink`. Each `ParentLink` contains a pointer to an object
     * (the prototype) and a pointer to the next `ParentLink`. This forms a chain
     * (or more accurately, a directed acyclic graph) that is traversed during
     * attribute lookup. This structure allows for multiple inheritance.
     */

    /**
     * @brief Constructs a new link in the prototype chain.
     * @param context The current execution context.
     * @param parent A pointer to the previous link in the chain (the next prototype to check).
     * @param object The actual prototype object this link represents.
     */
#include <unordered_map>
#include <mutex>
#include <execinfo.h>
#include <stdlib.h>
std::unordered_map<const ParentLinkImplementation*, const ParentLinkImplementation*> dbg_parentLinks;
std::mutex dbg_mutex;

    ParentLinkImplementation::ParentLinkImplementation(
        ProtoContext* context,
        const ParentLinkImplementation* parent,
        const ProtoObject* object
    ) : Cell(context), parent(parent), object(object)
    {
    };

    /**
     * @brief Informs the GC about the cells this object holds references to.
     * A ParentLink holds two potential references: the next parent in the chain,
     * and the prototype object itself.
     */
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
        // Report the link to the next parent in the chain.
        if (this->parent)
        {
            method(context, self, this->parent);
        }

        // Report the prototype object this link represents.
        if (this->object) {
            if (reinterpret_cast<uintptr_t>(this->object) & 1) {
                std::cerr << "BINGO! Tagged pointer in this->object: " << this->object 
                          << " isCellPointer=" << ProtoObject::isCellPointer(this->object) << "\n";
            }
        }
        if (this->object && ProtoObject::isCellPointer(this->object))
        {
            method(context, self, ProtoObject::asCellPointer(this->object));
        }
    }

    const ProtoObject* ParentLinkImplementation::implAsObject(ProtoContext* context) const
    {
        // A ParentLink is an internal implementation detail and should not be exposed as a public object.
        return PROTO_NONE;
    }

    /**
     * @brief Returns the prototype object this link points to.
     */
    const ProtoObject* ParentLinkImplementation::getObject(ProtoContext* context) const
    {
        return this->object;
    };

    /**
     * @brief Returns the next link in the prototype chain.
     */
    const ParentLinkImplementation* ParentLinkImplementation::getParent(ProtoContext* context) const
    {
        return this->parent;
    };
};
