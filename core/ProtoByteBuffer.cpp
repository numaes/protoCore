/*
 * ProtoByteBuffer.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto
{
    // --- Constructor ---
    // Uses a member initialization list for greater efficiency and clarity.
    ProtoByteBufferImplementation::ProtoByteBufferImplementation(
        ProtoContext* context,
        char* buffer,
        const unsigned long size,
        const bool freeOnExit
    ) : Cell(context), buffer(buffer), size(size), freeOnExit(false)
    {
        if (buffer)
        {
            // If an external buffer is provided, we simply wrap it.
            // We do not own this memory.
            this->buffer = buffer;
            this->freeOnExit = freeOnExit;
        }
        else
        {
            // If no buffer is provided, we create a new one.
            // We own this memory and must free it.
            // Using 'new char[size]' is safer and more idiomatic in C++ than 'malloc'.
            this->buffer = new char[size];
            this->freeOnExit = true;
        }
    }

    // --- Destructor ---
    ProtoByteBufferImplementation::~ProtoByteBufferImplementation()
    = default;

    // --- Access Methods ---

    // Private helper function to normalize the index.
    // This avoids code duplication and improves readability.
    static bool normalizeIndex(const ProtoByteBufferImplementation* self, int& index)
    {
        if (self->size == 0)
        {
            return false; // There are no valid indices in an empty buffer.
        }

        // Handling of negative indices (from the end of the buffer).
        if (index < 0)
        {
            index += static_cast<int>(self->size);
        }

        // Bounds checking. If it is out of range, it is not valid.
        // Using 'unsigned long' for comparison avoids sign issues.
        if (index < 0 || index >= self->size)
        {
            return false;
        }

        return true;
    }

    char ProtoByteBufferImplementation::implGetAt(ProtoContext* context, int index) const
    {
        // We use the helper function to validate and normalize the index.
        if (normalizeIndex(this, index))
        {
            return this->buffer[index];
        }
        // If the index is invalid, we return 0 as a default value.
        return 0;
    }

    void ProtoByteBufferImplementation::implSetAt(ProtoContext* context, int index, char value) const
    {
        // We only write if the index is valid.
        if (normalizeIndex(this, index))
        {
            this->buffer[index] = value;
        }
    }

    // --- Garbage Collector (GC) Methods ---

    void ProtoByteBufferImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            Cell* cell
        )
    ) const override
    {
        // This method is intentionally left empty.
        // A ProtoByteBufferImplementation manages a raw C++ buffer, which does not
        // contain any references to other Proto 'Cells' that the GC needs to track.
    };

    void ProtoByteBufferImplementation::finalize(ProtoContext* context) const override
    {
        // This is the effective destructor called by the GC.
        // We free the memory only if we own it.
        if (this->buffer && this->freeOnExit) {
            delete[] this->buffer;
            const_cast<ProtoByteBufferImplementation*>(this)->buffer = nullptr;
        }
    };


    // --- Interface Methods ---

    const ProtoObject* ProtoByteBufferImplementation::implAsObject(ProtoContext* context) const override
    {
        ProtoObjectPointer p{};
        p.byteBufferImplementation = this;
        p.op.pointer_tag = POINTER_TAG_BYTE_BUFFER;
        return p.oid.oid;
    }

    const ProtoByteBuffer* ProtoByteBufferImplementation::asByteBuffer(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.byteBufferImplementation = this;
        p.op.pointer_tag = POINTER_TAG_BYTE_BUFFER;
        return p.byteBuffer;
    }

    unsigned long ProtoByteBufferImplementation::getHash(ProtoContext* context) const override
    {
        // The hash of a Cell is derived directly from its memory address.
        // This provides a fast and unique identifier for the object.
        ProtoObjectPointer p{};
        p.byteBufferImplementation = this;

        return p.asHash.hash;
    }

    unsigned long ProtoByteBufferImplementation::implGetSize(ProtoContext* context) const
    {
        return this->size;
    }

    char* ProtoByteBufferImplementation::implGetBuffer(ProtoContext* context) const
    {
        return this->buffer;
    }

} // namespace proto