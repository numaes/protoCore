#include "../headers/proto_internal.h"

namespace proto
{
    // --- ProtoMethodCell ---

    ProtoMethodCell::ProtoMethodCell(ProtoContext* context, const ProtoObject* selfObject, ProtoMethod methodTarget) :
        Cell(context), self(selfObject), method(methodTarget)
    {
    };

    const ProtoObject* ProtoMethodCell::implInvoke(
        ProtoContext* context,
        const ProtoList* args,
        const ProtoSparseList* kwargs
    ) const
    {
        return this->method(context, const_cast<ProtoObject*>(this->self), static_cast<ParentLink*>(nullptr), const_cast<ProtoList*>(args), const_cast<ProtoSparseList*>(kwargs));
    }

    const ProtoObject* ProtoMethodCell::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.methodCellImplementation = this;
        p.op.pointer_tag = POINTER_TAG_METHOD;
        return p.oid;
    }

    unsigned long ProtoMethodCell::getHash(ProtoContext* context) const
    {
        return Cell::getHash(context);
    }

    void ProtoMethodCell::finalize(ProtoContext* context) const
    {
        // No finalization action is required.
    }

    void ProtoMethodCell::processReferences(
        ProtoContext* context,
        void* target,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)
    ) const
    {
        if (this->self && this->self->isCell(context))
        {
            method(context, target, this->self->asCell(context));
        }
    };

    const ProtoObject* ProtoMethodCell::implGetSelf(ProtoContext* context) const
    {
        return this->self;
    }

    ProtoMethod ProtoMethodCell::implGetMethod(ProtoContext* context) const
    {
        return this->method;
    }

} // namespace proto
