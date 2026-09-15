#include "../headers/proto_internal.h"
#include <cmath>
#include <functional>
#include <limits>

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
        // The hash must agree with equality.  -0.0 == 0.0, so both hash as
        // 0.0.  NaN has many bit patterns (sign and payload; x86 0.0/0.0 is a
        // negative NaN, std::nan("") a positive one) and every one of them is
        // the same set element, so all NaNs hash as the canonical quiet NaN.
        // Other values keep their bit-pattern hash.
        double value = doubleValue;
        if (std::isnan(value)) {
            value = std::numeric_limits<double>::quiet_NaN();
        } else if (value == 0.0) {
            value = 0.0;
        }
        return std::hash<double>{}(value);
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
