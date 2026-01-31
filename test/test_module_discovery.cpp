/*
 * test_module_discovery.cpp - Tests for Unified Module Discovery and Provider System.
 */

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include <memory>
#include <string>

using namespace proto;

namespace {

class TestProvider : public ModuleProvider {
public:
    TestProvider(std::string guid, std::string alias, std::string loadPath)
        : guid_(std::move(guid)), alias_(std::move(alias)), loadPath_(std::move(loadPath)) {}

    const ProtoObject* tryLoad(const std::string& logicalPath, ProtoContext* ctx) override {
        if (logicalPath == loadPath_) {
            return ctx->newObject(false);
        }
        return PROTO_NONE;
    }
    const std::string& getGUID() const override { return guid_; }
    const std::string& getAlias() const override { return alias_; }

private:
    std::string guid_;
    std::string alias_;
    std::string loadPath_;
};

} // anonymous namespace

class ModuleDiscoveryTest : public ::testing::Test {
protected:
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;

    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ModuleDiscoveryTest, ProviderRegistry_RegisterAndFindByAlias) {
    auto provider = std::make_unique<TestProvider>("guid-a", "alias_a", "test");
    ModuleProvider* raw = provider.get();
    ProviderRegistry::instance().registerProvider(std::move(provider));
    ModuleProvider* found = ProviderRegistry::instance().findByAlias("alias_a");
    ASSERT_EQ(found, raw);
}

TEST_F(ModuleDiscoveryTest, ProviderRegistry_RegisterAndFindByGUID) {
    auto provider = std::make_unique<TestProvider>("guid-b", "alias_b", "test");
    ModuleProvider* raw = provider.get();
    ProviderRegistry::instance().registerProvider(std::move(provider));
    ModuleProvider* found = ProviderRegistry::instance().findByGUID("guid-b");
    ASSERT_EQ(found, raw);
}

TEST_F(ModuleDiscoveryTest, ProviderRegistry_GetProviderForSpec_Alias) {
    auto provider = std::make_unique<TestProvider>("guid-c", "my_alias", "x");
    ModuleProvider* raw = provider.get();
    ProviderRegistry::instance().registerProvider(std::move(provider));
    ModuleProvider* found = ProviderRegistry::instance().getProviderForSpec("provider:my_alias");
    ASSERT_EQ(found, raw);
}

TEST_F(ModuleDiscoveryTest, ProviderRegistry_GetProviderForSpec_GUID) {
    auto provider = std::make_unique<TestProvider>("my-guid-123", "", "y");
    ModuleProvider* raw = provider.get();
    ProviderRegistry::instance().registerProvider(std::move(provider));
    ModuleProvider* found = ProviderRegistry::instance().getProviderForSpec("provider:my-guid-123");
    ASSERT_EQ(found, raw);
}

TEST_F(ModuleDiscoveryTest, ResolutionChain_GetReturnsNonEmpty) {
    const ProtoObject* chain = space.getResolutionChain();
    ASSERT_NE(chain, PROTO_NONE);
    ASSERT_NE(chain, nullptr);
    const ProtoList* list = chain->asList(ctx);
    ASSERT_NE(list, nullptr);
    ASSERT_GT(list->getSize(ctx), 0u);
}

TEST_F(ModuleDiscoveryTest, ResolutionChain_FirstEntryIsDotOnUnix) {
#if !defined(_WIN32)
    const ProtoObject* chain = space.getResolutionChain();
    ASSERT_NE(chain, PROTO_NONE);
    const ProtoList* list = chain->asList(ctx);
    ASSERT_NE(list, nullptr);
    const ProtoObject* first = list->getAt(ctx, 0);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->isString(ctx));
    std::string s;
    first->asString(ctx)->toUTF8String(ctx, s);
    ASSERT_EQ(s, ".");
#endif
}

TEST_F(ModuleDiscoveryTest, GetImportModule_NoProviderReturnsNone) {
    const ProtoObject* result = space.getImportModule("nonexistent_module_xyz", "exports");
    ASSERT_TRUE(result == PROTO_NONE || result == nullptr);
}

TEST_F(ModuleDiscoveryTest, DISABLED_GetImportModule_ProviderReturnsModule) {
    auto provider = std::make_unique<TestProvider>("guid-load", "load_alias", "my_module");
    ProviderRegistry::instance().registerProvider(std::move(provider));

    const ProtoList* chain = ctx->newList();
    ASSERT_NE(chain, nullptr);
    chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:load_alias"));
    ASSERT_NE(chain, nullptr);
    space.setResolutionChain(chain->asObject(ctx));

    const ProtoObject* result = space.getImportModule("my_module", "exports");
    ASSERT_NE(result, PROTO_NONE);
    ASSERT_NE(result, nullptr);

    const ProtoString* key = ProtoString::fromUTF8String(ctx, "exports");
    ASSERT_NE(key, nullptr);
    const ProtoObject* exports = result->getAttribute(ctx, key);
    ASSERT_NE(exports, PROTO_NONE);
    ASSERT_NE(exports, nullptr);
}

TEST_F(ModuleDiscoveryTest, DISABLED_GetImportModule_CacheHit) {
    auto provider = std::make_unique<TestProvider>("guid-cache", "cache_alias", "cached_mod");
    ProviderRegistry::instance().registerProvider(std::move(provider));

    const ProtoList* chain = ctx->newList();
    ASSERT_NE(chain, nullptr);
    chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:cache_alias"));
    space.setResolutionChain(chain->asObject(ctx));

    const ProtoObject* first = space.getImportModule("cached_mod", "exports");
    ASSERT_NE(first, PROTO_NONE);
    const ProtoObject* second = space.getImportModule("cached_mod", "exports");
    ASSERT_NE(second, PROTO_NONE);

    const ProtoString* key = ProtoString::fromUTF8String(ctx, "exports");
    const ProtoObject* exp1 = first->getAttribute(ctx, key);
    const ProtoObject* exp2 = second->getAttribute(ctx, key);
    ASSERT_EQ(exp1, exp2);
}

TEST_F(ModuleDiscoveryTest, ProtoString_ToUTF8String) {
    const ProtoString* s = ProtoString::fromUTF8String(ctx, "hello");
    ASSERT_NE(s, nullptr);
    std::string out;
    s->toUTF8String(ctx, out);
    ASSERT_EQ(out, "hello");
}
