/*
 * ModuleResolver.cpp - getImportModule: resolve and load module via resolution chain and SharedModuleCache.
 */

#include "../headers/protoCore.h"
#include "ModuleCache.h"
#include "ModuleProvider.h"
#include "../headers/proto_internal.h"
#include <memory>
#include <mutex>
#include <string>

namespace proto {

const ProtoObject* getImportModuleImpl(ProtoSpace* space, ProtoContext* context, const char* logicalPath, const char* attrName2create) {
    if (!space || !context || !logicalPath || !attrName2create) return PROTO_NONE;

    const std::string key(logicalPath);
    ProtoContext* ctx = context;

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

        // Select this entry's provider FIRST: under a provider-qualified
        // identity the cache cannot be probed before a provider is known.
        ModuleProvider* provider = nullptr;
        std::unique_ptr<FileSystemProvider> owned;   // a filesystem entry's provider
        if (entryStr.size() >= 9 && entryStr.compare(0, 9, "provider:") == 0) {
            provider = ProviderRegistry::instance().getProviderForSpec(entryStr);
            if (!provider) {
                if (diag) {
                    fprintf(stderr, "DEBUG: [UMD]   Provider NOT FOUND for spec: %s\n", entryStr.c_str());
                }
                continue;
            }
            if (diag) {
                fprintf(stderr, "DEBUG: [UMD]   Using provider: %s (GUID=%s)\n",
                        provider->getAlias().c_str(), provider->getGUID().c_str());
            }
        } else {
            owned = std::make_unique<FileSystemProvider>(entryStr);
            provider = owned.get();
        }

        // P3 D11: a module's identity is provider + path + version, so the cache
        // cannot be probed before an entry has selected a provider.  Moving the
        // probe here also fixes a real ordering bug: a module already loaded from
        // a LATER chain entry no longer shadows an EARLIER entry that can serve
        // it.  Chain order is the user's stated precedence.
        //
        // The version is empty: there is no module manifest yet, and "" is the
        // reserved, permanent identity of a module that declares none, so this
        // key is byte-identical once versions exist (P3 D11).
        const ModuleIdentity id =
            ModuleIdentity::unversioned(provider->getGUID(), key);

        if (const ProtoObject* cached = sharedModuleCacheGet(id)) {
            if (diag) {
                fprintf(stderr, "DEBUG: [UMD]   CACHE HIT at entry[%lu]\n", i);
            }
            module = cached;
            break;
        }

        module = provider->tryLoad(key, ctx);
        if (module != nullptr && module != PROTO_NONE) {
            if (diag) {
                fprintf(stderr, "DEBUG: [UMD]   SUCCESS: Module loaded from entry[%lu]\n", i);
            }
            sharedModuleCacheInsert(id, module);
            break;
        }
        module = nullptr;
    }

    if (!module || module == PROTO_NONE) {
        if (diag) {
            fprintf(stderr, "DEBUG: [UMD] FAILURE: Module %s not found in any entry\n", logicalPath);
        }
        return PROTO_NONE;
    }

    {
        std::lock_guard<std::mutex> lock(space->moduleRootsMutex);
        // Ensure the module is rooted in this space.  A module found in the
        // cache may have been loaded by another space; rooting it here is what
        // the pre-P3 cache-hit branch did and what keeps a cross-space import
        // alive.
        if (std::find(space->moduleRoots.begin(), space->moduleRoots.end(), module) == space->moduleRoots.end()) {
            space->moduleRoots.push_back(module);
        }
    }

    // GC critical section: `wrapper` and `attrName` are held in C++ locals
    // across newObject + addParent + fromUTF8String + setAttribute, each of
    // which allocates.  Without the guard, a sweep landing between any two
    // could orphan one of them.
    //
    // The wrapper is built ONCE, for both the cache hit and the fresh load.
    // Before P3 these were two branches that differed: the cache-hit branch
    // re-parented the wrapper to space->objectPrototype and the fresh-load
    // branch did not, so the same call returned a wrapper with a different
    // prototype chain depending on whether the module happened to be cached.
    // The addParent form is kept, because a wrapper without it is the odd one
    // out, and the nondeterminism goes away.
    ProtoContext::CriticalSection cs(ctx);
    const ProtoObject* wrapper = ctx->newObject(false);
    if (!wrapper) return PROTO_NONE;
    if (space->objectPrototype) {
        wrapper = wrapper->addParent(ctx, space->objectPrototype);
        if (!wrapper) return PROTO_NONE;
    }
    const ProtoString* attrName = ProtoString::fromUTF8(ctx, attrName2create);
    if (!attrName) return PROTO_NONE;
    wrapper = wrapper->setAttribute(ctx, attrName, module);
    return wrapper;
}

} // namespace proto
