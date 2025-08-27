#include "../headers/proto_internal.h"

namespace proto
{
    // --- ProtoMethodCellImplementation ---

    /**
     * @brief Constructor
     * @param context The current execution context.
     * @param method The method to be used.
     */

    ProtoMethodCellImplementation::ProtoMethodCellImplementation(ProtoContext* context, const ProtoMethod method): Cell(
        context)
    {
        this->method = method;
    };

    /**
     * @brief Invokes the native C method encapsulated by this cell.
     *
     * This function acts as a bridge between the Proto object system
     * and native C code. It delegates the call to the 'method' function pointer
     * that was stored during construction.
     *
     * @param context The current execution context.
     * @param args A list of positional arguments for the function.
     * @param kwargs A sparse list of keyword arguments.
     * @return The ProtoObject resulting from the execution of the native method.
     */
    ProtoObject* ProtoMethodCellImplementation::implInvoke(
        ProtoContext* context,
        ProtoList* args,
        ProtoSparseList* kwargs
    )
    {
        // The main logic is to simply call the stored function pointer.
        // It is assumed that this->method is not null, which should be guaranteed in the constructor.
        return this->method(context, this->implAsObject(context), static_cast<ParentLink*>(nullptr), args, kwargs);
    }

    /**
     * @brief Returns the representation of this cell as a generic ProtoObject.
     *
     * This method is essential for the cell to be handled as a first-class object
     * within the Proto system. It assigns the 'this' pointer and
     * sets the correct pointer tag to identify it as a method.
     *
     * @param context The current execution context.
     * @return A ProtoObject representing this method cell.
     */
    ProtoObject* ProtoMethodCellImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p{};
        p.methodCellImplementation = this;
        p.op.pointer_tag = POINTER_TAG_METHOD; // Tag to identify method cells
        return p.oid.oid;
    }

    /**
     * @brief Gets the hash code for this cell.
     *
     * Reuses the implementation from the base class 'Cell', which generates a hash
     * based on the object's memory address. This is efficient and guarantees
     * the uniqueness of the hash for each cell instance.
     *
     * @param context The current execution context.
     * @return The hash value as an unsigned long.
     */
    unsigned long ProtoMethodCellImplementation::getHash(ProtoContext* context)
    {
        return Cell::getHash(context);
    }

    /**
     * @brief Finalizer for the method cell.
     *
     * This method is called before the garbage collector reclaims the object's memory.
     * In this case, no special cleanup is needed, as the function pointer
     * is not a resource that needs to be manually released.
     *
     * @param context The current execution context.
     */
    void ProtoMethodCellImplementation::finalize(ProtoContext* context)
    {
        // No finalization action is required.
    }

    /**
     * @brief Processes references for the garbage collector.
     *
     * This implementation is empty because a ProtoMethodCellImplementation
     * contains a pointer to a C function, not references to other 'Cells' that
     * the garbage collector needs to track. It is crucial that this method
     * does not process 'this' to avoid infinite loops in the GC.
     *
     * @param context The current execution context.
     * @param self Pointer to the object being processed.
     * @param method GC callback function to mark references.
     */
    void ProtoMethodCellImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    )
    {
        // This cell contains no references to other cells, so the body is empty.
    };

    ProtoObject* ProtoMethodCellImplementation::implGetSelf(ProtoContext* context) {
        // TODO: Implement actual logic to return the 'self' object associated with the method.
        return nullptr;
    }

    ProtoMethod ProtoMethodCellImplementation::implGetMethod(ProtoContext* context) {
        // TODO: Implement actual logic to return the wrapped function pointer.
        return nullptr;
    }
} // namespace proto
