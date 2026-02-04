/*
 * ModuleResolver.cpp - getImportModule: resolve and load module via resolution chain and SharedModuleCache.
 */

#include "../headers/protoCore.h"
#include "ModuleCache.h"
#include "ModuleProvider.h"
#include "../headers/proto_internal.h"
#include <mutex>
#include <string>

namespace proto {

const ProtoObject* getImportModuleImpl(ProtoSpace* space, const char* logicalPath, const char* attrName2create) {
    if (!space || !logicalPath || !attrName2create) return PROTO_NONE;

    const std::string key(logicalPath);
    ProtoContext* ctx = space->rootContext;

    const ProtoObject* cached = sharedModuleCacheGet(key);
    if (cached) {
        const ProtoObject* wrapper = ctx->newObject(false);
        if (!wrapper) return PROTO_NONE;
        const ProtoString* attrName = ProtoString::fromUTF8String(ctx, attrName2create);
        if (!attrName) return PROTO_NONE;
        wrapper = wrapper->setAttribute(ctx, attrName, cached);
        return wrapper;
    }

    const ProtoObject* chainObj = space->getResolutionChain();
    if (!chainObj || chainObj == PROTO_NONE) return PROTO_NONE;

    const ProtoList* chain = chainObj->asList(ctx);
    if (!chain) return PROTO_NONE;
    const unsigned long chainSize = chain->getSize(ctx);
    const ProtoObject* module = nullptr;

    for (unsigned long i = 0; i < chainSize; ++i) {
        const ProtoObject* entryObj = chain->getAt(ctx, static_cast<int>(i));
        if (!entryObj || !entryObj->isString(ctx)) continue;

        std::string entryStr;
        entryObj->asString(ctx)->toUTF8String(ctx, entryStr);

        if (entryStr.size() >= 9 && entryStr.compare(0, 9, "provider:") == 0) {
            ModuleProvider* provider = ProviderRegistry::instance().getProviderForSpec(entryStr);
            if (provider) {
                module = provider->tryLoad(key, ctx);
            }
        } else {
            FileSystemProvider fsProvider(entryStr);
            module = fsProvider.tryLoad(key, ctx);
        }
        if (module != nullptr && module != PROTO_NONE) break;
    }

    if (!module || module == PROTO_NONE) return PROTO_NONE;

    sharedModuleCacheInsert(key, module);

    {
        std::lock_guard<std::mutex> lock(space->moduleRootsMutex);
        space->moduleRoots.push_back(module);
    }

    const ProtoObject* wrapper = ctx->newObject(false);
    if (!wrapper) return PROTO_NONE;
    const ProtoString* attrName = ProtoString::fromUTF8String(ctx, attrName2create);
    if (!attrName) return PROTO_NONE;
    wrapper = wrapper->setAttribute(ctx, attrName, module);
    return wrapper;
}

} // namespace proto
