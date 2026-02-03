/*
 * ProviderRegistry.cpp - Singleton registry of ModuleProviders.
 */

#include "../headers/protoCore.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace proto {

struct ProviderRegistry::Impl {
    std::vector<std::unique_ptr<ModuleProvider>> providers;
    std::map<std::string, ModuleProvider*> byAlias;
    std::map<std::string, ModuleProvider*> byGUID;
    std::mutex mutex;
};

ProviderRegistry::ProviderRegistry() : impl(std::make_unique<Impl>()) {}

ProviderRegistry::~ProviderRegistry() = default;

ProviderRegistry& ProviderRegistry::instance() {
    static ProviderRegistry s_instance;
    return s_instance;
}

void ProviderRegistry::registerProvider(std::unique_ptr<ModuleProvider> provider) {
    if (!provider) return;
    ModuleProvider* raw = provider.get();
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->providers.push_back(std::move(provider));
    const std::string& guid = raw->getGUID();
    const std::string& alias = raw->getAlias();
    impl->byGUID[guid] = raw;
    if (!alias.empty()) {
        impl->byAlias[alias] = raw;
    }
}

ModuleProvider* ProviderRegistry::findByAlias(const std::string& alias) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto it = impl->byAlias.find(alias);
    return it != impl->byAlias.end() ? it->second : nullptr;
}

ModuleProvider* ProviderRegistry::findByGUID(const std::string& guid) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto it = impl->byGUID.find(guid);
    return it != impl->byGUID.end() ? it->second : nullptr;
}

ModuleProvider* ProviderRegistry::getProviderForSpec(const std::string& spec) {
    const char prefix[] = "provider:";
    const size_t prefixLen = sizeof(prefix) - 1;
    if (spec.size() < prefixLen || spec.compare(0, prefixLen, prefix) != 0) {
        return nullptr;
    }
    std::string key = spec.substr(prefixLen);
    if (key.empty()) return nullptr;
    ModuleProvider* byAlias = findByAlias(key);
    if (byAlias) return byAlias;
    return findByGUID(key);
}

} // namespace proto
