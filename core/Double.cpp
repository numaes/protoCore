#include "../headers/proto_internal.h"
#include <functional>

namespace proto
{

    //================================================================================
    // DoubleImplementation
    //================================================================================

    DoubleImplementation::DoubleImplementation(ProtoContext* context, double val)
        : Cell(context), doubleValue(val)
    {
    }

    unsigned long DoubleImplementation::getHash(ProtoContext* context) const
    {
        return std::hash<double>{}(doubleValue);
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

    const ProtoObject* DoubleImplementation::implAsObject(ProtoContext* context) const
    {
        ProtoObjectPointer p;
        p.doubleImplementation = this;
        p.op.pointer_tag = POINTER_TAG_DOUBLE;
        return p.oid;
    }

} // namespace proto
