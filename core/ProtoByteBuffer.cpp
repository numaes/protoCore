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
    ProtoByteBufferImplementation::ProtoByteBufferImplementation(
        ProtoContext* context,
        char* buffer,
        const unsigned long size,
        const bool freeOnExit
    ) : Cell(context), buffer(buffer), size(size), freeOnExit(false)
    {
        if (buffer)
        {
            this->buffer = buffer;
            this->freeOnExit = freeOnExit;
        }
        else
        {
            this->buffer = new char[size];
            this->freeOnExit = true;
        }
    }

    // --- Destructor ---
    ProtoByteBufferImplementation::~ProtoByteBufferImplementation() = default;

    // --- Access Methods ---

    static bool normalizeIndex(const ProtoByteBufferImplementation* self, int& index)
    {
        if (self->size == 0) return false;
        if (index < 0) index += static_cast<int>(self->size);
        if (index < 0 || index >= self->size) return false;
        return true;
    }

    char ProtoByteBufferImplementation::implGetAt(ProtoContext* context, int index) const
    {
        if (normalizeIndex(this, index))
        {
            return this->buffer[index];
        }
        return 0;
    }

    // Corrected: Removed 'const' as this method modifies the object's state.
    void ProtoByteBufferImplementation::implSetAt(ProtoContext* context, int index, char value)
    {
        if (normalizeIndex(this, index))
        {
            this->buffer[index] = value;
        }
    }

    // --- Garbage Collector (GC) Methods ---

    // Corrected: Signature updated to match the base class.
    void ProtoByteBufferImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const override
    {
        // Intentionally empty. A byte buffer does not hold references to other Proto Cells.
    };

    // Corrected: Removed 'const' as this method modifies the object's state (deletes buffer).
    void ProtoByteBufferImplementation::finalize(ProtoContext* context) override
    {
        if (this->buffer && this->freeOnExit) {
            delete[] this->buffer;
            this->buffer = nullptr; // No const_cast needed anymore.
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
