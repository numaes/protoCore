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
        const ProtoArguments* args,
        const ProtoKeywordArguments* kwargs
    ) const
    {
        return this->method(context, this->self, nullptr, args, kwargs);
    }

    const ProtoObject* ProtoMethodCell::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p{};
        p.methodCellImplementation = this;
        p.op.pointer_tag = POINTER_TAG_METHOD;
        return p.oid;
    }

    proto_ulong ProtoMethodCell::getHash(ProtoContext* context) const
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
        if (const Cell* c = ProtoObject::asCellPointer(this->self))
        {
            method(context, target, c);
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
