/*
 * proto_internal.h
 *
 *  Created on: November 2017 - Redesign January 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */

#ifndef PROTO_INTERNAL_H
#define PROTO_INTERNAL_H

#include "../headers/proto.h"
#include <thread>
#include <memory>

// ... (other definitions)

namespace proto
{
    // ... (other class declarations)

    class ProtoByteBufferImplementation final : public Cell
    {
    public:
        ProtoByteBufferImplementation(
            ProtoContext* context,
            char* buffer,
            unsigned long size,
            bool freeOnExit = false
        );
        ~ProtoByteBufferImplementation() override;

        char implGetAt(ProtoContext* context, int index) const;
        void implSetAt(ProtoContext* context, int index, char value); // Removed const
        unsigned long implGetSize(ProtoContext* context) const;
        char* implGetBuffer(ProtoContext* context) const;
        const ProtoObject* implAsObject(ProtoContext* context) const override;
        const ProtoByteBuffer* asByteBuffer(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const override;

        void finalize(ProtoContext* context) override; // Removed const
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, const Cell*)
        ) const override;

        char* buffer;
        unsigned long size;
        bool freeOnExit;
    };

    // ... (rest of the file)
}

#endif /* PROTO_INTERNAL_H */
