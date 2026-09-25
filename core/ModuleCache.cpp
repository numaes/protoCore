/*
 * ModuleCache.cpp - Thread-safe SharedModuleCache for getImportModule.
 */

#include "../headers/protoCore.h"
#include "ModuleCache.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <shared_mutex>
#include <string>

namespace proto {

namespace {

struct SharedModuleCache {
    std::map<std::string, const ProtoObject*> cache;
    mutable std::shared_mutex mutex;

    const ProtoObject* get(const std::string& key) {
        std::shared_lock lock(mutex);
        auto it = cache.find(key);
        return it != cache.end() ? it->second : nullptr;
    }

    void insert(const std::string& key, const ProtoObject* value) {
        if (!value) return;
        std::unique_lock lock(mutex);
        cache[key] = value;
    }
};

SharedModuleCache& getCache() {
    static SharedModuleCache s_cache;
    return s_cache;
}

} // anonymous namespace

const ProtoObject* sharedModuleCacheGet(const ModuleIdentity& id) {
    return getCache().get(id.asKey());
}

void sharedModuleCacheInsert(const ModuleIdentity& id, const ProtoObject* module) {
    if (std::getenv("PROTO_RESOLVE_DIAG")) {
        // Render the '\x1F' separators as '|' so the log stays readable.
        std::string shown = id.asKey();
        for (char& ch : shown) if (ch == '\x1F') ch = '|';
        fprintf(stderr, "DEBUG: sharedModuleCacheInsert(%s, %p)\n", shown.c_str(), (void*)module);
    }
    getCache().insert(id.asKey(), module);
}

} // namespace proto
