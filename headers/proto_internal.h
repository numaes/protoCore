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
    // Forward declaration
    class ProtoThreadExtension;

    struct AttributeCacheEntry
    {
        const ProtoObject* object;
        const ProtoString* attributeName;
        const ProtoObject* value;
    };

    class ProtoThreadImplementation final : public Cell
    {
    public:
        ProtoThreadImplementation(
            ProtoContext* context,
            const ProtoString* name,
            ProtoSpace* space,
            ProtoMethod method = nullptr,
            const ProtoList* unnamedArgList = nullptr,
            const ProtoSparseList* kwargs = nullptr
        );
        ~ProtoThreadImplementation() override;

        // ... methods ...

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*callBackMethod)(ProtoContext*, void*, Cell*)
        ) const override;

        // --- Member Data (Primary Cell) ---
        ProtoSpace* space;
        ProtoContext* currentContext;
        int state;
        std::atomic<int> unmanagedCount;
        const ProtoString* name;
        ProtoThreadExtension* extension; // Pointer to the second cell
    };

    // This new class holds the rest of the data
    class ProtoThreadExtension final : public Cell
    {
    public:
        ProtoThreadExtension(ProtoContext* context);
        ~ProtoThreadExtension() override;

        void finalize(ProtoContext* context) const override;
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*callBackMethod)(ProtoContext*, void*, Cell*)
        ) const override;

        // --- Member Data (Extension Cell) ---
        std::unique_ptr<std::thread> osThread;
        AttributeCacheEntry* attributeCache;
        Cell* freeCells;
    };

    // ... (rest of the file)

    // Update static_asserts
    static_assert(sizeof(ProtoThreadImplementation) <= 64, "ProtoThreadImplementation exceeds 64 bytes!");
    static_assert(sizeof(ProtoThreadExtension) <= 64, "ProtoThreadExtension exceeds 64 bytes!");
}

#endif /* PROTO_INTERNAL_H */
