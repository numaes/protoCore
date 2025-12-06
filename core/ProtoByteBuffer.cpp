/*
 * ProtoByteBuffer.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto
{
    // --- Constructor and Destructor ---
    ProtoByteBufferImplementation::ProtoByteBufferImplementation(
        ProtoContext* context,
        char* buffer,
        const unsigned long size,
        const bool freeOnExit
    ) : Cell(context), buffer(buffer), size(size), freeOnExit(freeOnExit)
    {
        if (!buffer)
        {
            this->buffer = new char[size];
            this->freeOnExit = true;
        }
    }

    ProtoByteBufferImplementation::~ProtoByteBufferImplementation() = default;

    // --- Helper Function ---
    static bool normalizeIndex(const ProtoByteBufferImplementation* self, int& index)
    {
        if (self->size == 0) return false;
        if (index < 0) index += static_cast<int>(self->size);
        if (index < 0 || static_cast<unsigned long>(index) >= self->size) return false;
        return true;
    }

    // --- Method Implementations ---

    char ProtoByteBufferImplementation::implGetAt(ProtoContext* context, int index) const
    {
        if (normalizeIndex(this, index))
        {
            return this->buffer[index];
        }
        return 0;
    }

    void ProtoByteBufferImplementation::implSetAt(ProtoContext* context, int index, char value)
    {
        if (normalizeIndex(this, index))
        {
            this->buffer[index] = value;
        }
    }

    void ProtoByteBufferImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
        )
    ) const
    {
        // A byte buffer does not hold references to other Proto Cells.
    }

    void ProtoByteBufferImplementation::finalize(ProtoContext* context) const
    {
        if (this->buffer && this->freeOnExit) {
            delete[] this->buffer;
            // It's good practice to nullify pointers after deletion,
            // but since this is const, we need a const_cast.
            // This is an acceptable use of const_cast as it's in a "destructor-like" method.
            const_cast<ProtoByteBufferImplementation*>(this)->buffer = nullptr;
        }
    }

    const ProtoObject* ProtoByteBufferImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.byteBufferImplementation = this;
        p.op.pointer_tag = POINTER_TAG_BYTE_BUFFER;
        return p.oid;
    }

    const ProtoByteBuffer* ProtoByteBufferImplementation::asByteBuffer(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.byteBufferImplementation = this;
        p.op.pointer_tag = POINTER_TAG_BYTE_BUFFER;
        return p.byteBuffer;
    }

    unsigned long ProtoByteBufferImplementation::getHash(ProtoContext* context) const
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
