#include "../headers/proto_internal.h"

namespace proto
{

    //================================================================================
    // DoubleImplementation
    //================================================================================

    DoubleImplementation::DoubleImplementation(ProtoContext* context, double doubleValue)
        : Cell(context), doubleValue(doubleValue)
    {
    }

    DoubleImplementation::~DoubleImplementation()
    {
    }

    unsigned long DoubleImplementation::getHash(ProtoContext* context) const
    {
        // Implementation will go here.
        return 0;
    }

    void DoubleImplementation::finalize(ProtoContext* context) const
    {
        // No special finalization needed for Double.
    }

    void DoubleImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, const Cell* cell)) const
    {
    }

    const ProtoObject* LargeIntegerImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.largeIntegerImplementation = this;
        p.op.pointer_tag = POINTER_TAG_DOUBLE;
        return p.oid.oid;
    }


    const ProtoObject* Integer::fromString(ProtoContext* context, const char* str, int base)
    {
        // To be implemented
        return PROTO_NONE;
    }

} // namespace proto
