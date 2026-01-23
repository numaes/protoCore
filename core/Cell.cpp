/*
 * Cell.cpp
 *
 *  Created on: 2020-05-01
 *      Author: gamarino
 *
 *  This file implements the foundational memory unit of the Proto runtime.
 */

#include "../headers/proto_internal.h"

namespace proto
{
    /**
     * @class Cell
     * @brief The fundamental building block for all heap-allocated objects in Proto.
     *
     * A Cell is a fixed-size (64-byte) block of memory that serves as the base
     * class for all complex objects managed by the garbage collector. This
     * uniform size simplifies memory allocation and eliminates fragmentation.
     *
     * The base Cell class provides default implementations for the virtual
     * methods required by the GC, such as `finalize` and `processReferences`.
     */

    /**
     * @brief Constructs a Cell and links it to the current context for GC tracking.
     * @param context The execution context that is allocating this cell.
     * @param next A pointer to form a linked list of cells, used by the context.
     */
    Cell::Cell(ProtoContext* context, Cell* next)
        : next_and_flags(reinterpret_cast<uintptr_t>(next))
    {
        // Each newly created Cell is immediately registered with the current context
        // for memory management and garbage collection tracking.
        if (context) {
            context->addCell2Context(this);
        }
    };

    /**
     * @brief Provides a default hash for a Cell, based on its memory address.
     * This ensures that every heap object has a unique, stable identifier.
     */
    unsigned long Cell::getHash(ProtoContext* context) const
    {
        return reinterpret_cast<uintptr_t>(this);
    }

    /**
     * @brief Overloads the `new` operator to hook into Proto's memory management.
     * Instead of calling the system allocator, it requests a new cell from the
     * current context's memory arena.
     */
    void* Cell::operator new(unsigned long size, ProtoContext* context)
    {
        return context->allocCell();
    };

    /**
     * @brief Used by the GC to traverse the object graph.
     * Each subclass must override this method to report all `Cell*` members it holds
     * by calling the provided `method` callback for each one. The base Cell holds
     * no references, so its implementation is empty.
     */
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
        // Base implementation holds no references to other cells.
    }
};