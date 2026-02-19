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

const ProtoObject* getImportModuleImpl(ProtoSpace* space, ProtoContext* context, const char* logicalPath, const char* attrName2create) {
    if (!space || !context || !logicalPath || !attrName2create) return PROTO_NONE;

    const std::string key(logicalPath);
    ProtoContext* ctx = context;

    const ProtoObject* cached = sharedModuleCacheGet(key);
    if (cached) {
        const ProtoObject* wrapper = ctx->newObject(false);
    if (space->objectPrototype) {
        wrapper = wrapper->addParent(ctx, space->objectPrototype);
    }
        if (!wrapper) return PROTO_NONE;
        const ProtoString* attrName = ProtoString::fromUTF8String(ctx, attrName2create);
        if (!attrName) return PROTO_NONE;
        wrapper = wrapper->setAttribute(ctx, attrName, cached);
        return wrapper;
    }

    const bool diag = std::getenv("PROTO_RESOLVE_DIAG");
    if (diag) {
        fprintf(stderr, "DEBUG: [UMD] getImportModule(logicalPath=%s)\n", logicalPath);
    }

    const ProtoObject* chainObj = space->getResolutionChain();
    if (!chainObj || chainObj == PROTO_NONE) {
        if (diag) {
            fprintf(stderr, "DEBUG: [UMD] No resolution chain found on space %p\n", (void*)space);
        }
        return PROTO_NONE;
    }

    const ProtoList* chain = chainObj->asList(ctx);
    if (!chain) {
        if (diag) {
            fprintf(stderr, "DEBUG: [UMD] resolutionChain is not a list\n");
        }
        return PROTO_NONE;
    }
    const unsigned long chainSize = chain->getSize(ctx);
    if (diag) {
        fprintf(stderr, "DEBUG: [UMD] resolutionChain size=%lu\n", chainSize);
    }
    const ProtoObject* module = nullptr;

    for (unsigned long i = 0; i < chainSize; ++i) {
        const ProtoObject* entryObj = chain->getAt(ctx, static_cast<int>(i));
        if (!entryObj || !entryObj->isString(ctx)) continue;

        std::string entryStr;
        entryObj->asString(ctx)->toUTF8String(ctx, entryStr);

        if (diag) {
            fprintf(stderr, "DEBUG: [UMD]  Attempting entry[%lu]: %s\n", i, entryStr.c_str());
        }

        if (entryStr.size() >= 9 && entryStr.compare(0, 9, "provider:") == 0) {
            ModuleProvider* provider = ProviderRegistry::instance().getProviderForSpec(entryStr);
            if (provider) {
                if (diag) {
                    fprintf(stderr, "DEBUG: [UMD]   Using provider: %s (GUID=%s)\n", provider->getAlias().c_str(), provider->getGUID().c_str());
                }
                module = provider->tryLoad(key, ctx);
            } else {
                if (diag) {
                    fprintf(stderr, "DEBUG: [UMD]   Provider NOT FOUND for spec: %s\n", entryStr.c_str());
                }
            }
        } else {
            FileSystemProvider fsProvider(entryStr);
            module = fsProvider.tryLoad(key, ctx);
        }
        if (module != nullptr && module != PROTO_NONE) {
            if (diag) {
                fprintf(stderr, "DEBUG: [UMD]   SUCCESS: Module loaded from entry[%lu]\n", i);
            }
            break;
        }
    }

    if (!module || module == PROTO_NONE) {
        if (diag) {
            fprintf(stderr, "DEBUG: [UMD] FAILURE: Module %s not found in any entry\n", logicalPath);
        }
        return PROTO_NONE;
    }

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
