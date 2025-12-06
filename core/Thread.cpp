/*
 * Thread.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <iostream>

namespace proto {

    namespace {
        void thread_main(
            ProtoContext* context,
            ProtoMethod method,
            const ProtoList* args,
            const ProtoSparseList* kwargs
        ) {
            try {
                method(context, reinterpret_cast<const ProtoObject*>(context->thread), nullptr, args, kwargs);
            } catch (const std::exception& e) {
                std::cerr << "Uncaught exception in thread: " << e.what() << std::endl;
            }
        }
    }

    //=========================================================================
    // ProtoThreadExtension
    //=========================================================================

    ProtoThreadExtension::ProtoThreadExtension(ProtoContext* context)
        : Cell(context), osThread(nullptr), freeCells(nullptr) {
        this->attributeCache = static_cast<AttributeCacheEntry*>(
            std::malloc(THREAD_CACHE_DEPTH * sizeof(AttributeCacheEntry)));
        for (int i = 0; i < THREAD_CACHE_DEPTH; ++i) {
            this->attributeCache[i] = {nullptr, nullptr, nullptr};
        }
    }

    ProtoThreadExtension::~ProtoThreadExtension() {
        std::free(this->attributeCache);
        if (osThread && osThread->joinable()) {
            osThread->join();
        }
        delete osThread;
    }

    void ProtoThreadExtension::finalize(ProtoContext* context) const {
        // Nothing to do here
    }

    void ProtoThreadExtension::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
            )
    ) const {
        // Nothing to do here
    }

    const ProtoObject* ProtoThreadExtension::implAsObject(ProtoContext* context) const {
        return PROTO_NONE;
    }


    //=========================================================================
    // ProtoThreadImplementation
    //=========================================================================

    const ProtoObject* ProtoThreadImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.threadImplementation = this;
        p.op.pointer_tag = POINTER_TAG_THREAD;
        return p.oid;
    }

    ProtoThreadImplementation::ProtoThreadImplementation(
        ProtoContext* context,
        const ProtoString* name,
        ProtoSpace* space,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs
    ) : Cell(context), name(name), space(space) {
        this->extension = new (context) ProtoThreadExtension(context);
        this->context = new (context) ProtoContext(space, nullptr, nullptr, nullptr, args, kwargs);
        this->context->thread = (ProtoThread*)this->asThread(context);
        this->extension->osThread = new std::thread(thread_main, this->context, mainFunction, args, kwargs);
    }

    ProtoThreadImplementation::~ProtoThreadImplementation() {
    }

    void ProtoThreadImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
            )
    ) const {
        if (this->extension) {
            method(context, self, this->extension);
        }
        if (this->name) {
            method(context, self, this->name->asCell(context));
        }
    }

    Cell* ProtoThreadImplementation::implAllocCell(ProtoContext* context) {
        if (!this->extension->freeCells) {
            this->implSynchToGC();
            this->extension->freeCells = this->space->getFreeCells((ProtoThread*)this->asThread(this->context));
        }

        if (!this->extension->freeCells) {
            return nullptr;
        }

        Cell* newCell = this->extension->freeCells;
        if (newCell) {
            this->extension->freeCells = newCell->next;
        }
        return newCell;
    }

    void ProtoThreadImplementation::implSynchToGC() {
        // This is a placeholder for a real implementation.
        // A real implementation would need to coordinate with the GC.
    }

    void ProtoThreadImplementation::implSetCurrentContext(ProtoContext* context) {
        this->context = context;
    }

    void ProtoThreadImplementation::finalize(ProtoContext* context) const {
        // Nothing to do here
    }

    unsigned long ProtoThreadImplementation::getHash(ProtoContext* context) const {
        return reinterpret_cast<uintptr_t>(this);
    }

    const ProtoThread* ProtoThreadImplementation::asThread(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.threadImplementation = this;
        p.op.pointer_tag = POINTER_TAG_THREAD;
        return p.thread;
    }

    //=========================================================================
    // ProtoThread API
    //=========================================================================

    const ProtoObject* ProtoThread::getName(ProtoContext* context) const {
        return reinterpret_cast<const ProtoObject*>(toImpl<const ProtoThreadImplementation>(this)->name);
    }
}
