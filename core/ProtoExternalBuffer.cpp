/*
 * ProtoExternalBuffer.cpp
 *
 * 64-byte header cell; contiguous segment via aligned_alloc.
 * Shadow GC: finalize() frees segment when the cell is collected.
 */

#include "../headers/proto_internal.h"
#include <cstdlib>
#include <cstring>

namespace proto {

    namespace {
        constexpr size_t kSegmentAlignment = 64;
    }

    ProtoExternalBufferImplementation::ProtoExternalBufferImplementation(
        ProtoContext* context,
        proto_ulong bufferSize
    ) : Cell(context), segment(nullptr), size(bufferSize)
    {
        if (bufferSize > 0) {
            void* p = alignedAlloc(kSegmentAlignment, bufferSize);
            if (p)
                segment = std::memset(p, 0, bufferSize);
        }
    }

    ProtoExternalBufferImplementation::~ProtoExternalBufferImplementation() {
        if (segment) {
            alignedFree(segment);
            segment = nullptr;
        }
    }

    void* ProtoExternalBufferImplementation::implGetRawPointer(ProtoContext* /*context*/) const {
        return segment;
    }

    proto_ulong ProtoExternalBufferImplementation::implGetSize(ProtoContext* /*context*/) const {
        return size;
    }

    const ProtoObject* ProtoExternalBufferImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.externalBufferImplementation = this;
        p.op.pointer_tag = POINTER_TAG_EXTERNAL_BUFFER;
        return p.oid;
    }

    void ProtoExternalBufferImplementation::processReferences(
        ProtoContext* /*context*/,
        void* /*self*/,
        void (*/*method*/)(ProtoContext*, void*, const Cell*)
    ) const {
        /* No references to other cells. */
    }

    void ProtoExternalBufferImplementation::finalize(ProtoContext* /*context*/) const {
        if (segment) {
            alignedFree(segment);
            segment = nullptr;
        }
    }

    proto_ulong ProtoExternalBufferImplementation::getHash(ProtoContext* /*context*/) const {
        return reinterpret_cast<uintptr_t>(segment) ^ size;
    }

}

namespace proto {

    void* ProtoExternalBuffer::getRawPointer(ProtoContext* context) const {
        return toImpl<const ProtoExternalBufferImplementation>(this)->implGetRawPointer(context);
    }

    proto_ulong ProtoExternalBuffer::getSize(ProtoContext* context) const {
        return toImpl<const ProtoExternalBufferImplementation>(this)->implGetSize(context);
    }

    const ProtoObject* ProtoExternalBuffer::asObject(ProtoContext* context) const {
        return toImpl<const ProtoExternalBufferImplementation>(this)->implAsObject(context);
    }

    proto_ulong ProtoExternalBuffer::getHash(ProtoContext* context) const {
        return toImpl<const ProtoExternalBufferImplementation>(this)->getHash(context);
    }

}
