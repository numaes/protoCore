/*
 * ProtoExternalPointer.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"

namespace proto {

    ProtoExternalPointerImplementation::ProtoExternalPointerImplementation(
        ProtoContext* context,
        void* p,
        void (*f)(void*)
    ) : Cell(context), pointer(p), finalizer(f)
    {
    }

    ProtoExternalPointerImplementation::~ProtoExternalPointerImplementation() {
        if (this->finalizer) {
            this->finalizer(this->pointer);
        }
    }

    void* ProtoExternalPointerImplementation::implGetPointer(ProtoContext* context) const {
        return this->pointer;
    }

    const ProtoObject* ProtoExternalPointerImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.externalPointerImplementation = this;
        p.op.pointer_tag = POINTER_TAG_EXTERNAL_POINTER;
        return p.oid;
    }

    void ProtoExternalPointerImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
            )
    ) const {
        // No references to other cells
    }

    void ProtoExternalPointerImplementation::finalize(ProtoContext* context) const {
        if (this->finalizer) {
            this->finalizer(this->pointer);
        }
    }

    unsigned long ProtoExternalPointerImplementation::getHash(ProtoContext* context) const {
        return reinterpret_cast<uintptr_t>(this->pointer);
    }

}
