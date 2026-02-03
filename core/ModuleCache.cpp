/*
 * ModuleCache.cpp - Thread-safe SharedModuleCache for getImportModule.
 */

#include "../headers/protoCore.h"
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

const ProtoObject* sharedModuleCacheGet(const std::string& logicalPath) {
    return getCache().get(logicalPath);
}

void sharedModuleCacheInsert(const std::string& logicalPath, const ProtoObject* module) {
    getCache().insert(logicalPath, module);
}

} // namespace proto
