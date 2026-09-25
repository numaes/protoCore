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
    const ProtoObject* result = space.getImportModule(ctx, "nonexistent_module_xyz", "exports");
    ASSERT_TRUE(result == PROTO_NONE || result == nullptr);
}

TEST_F(ModuleDiscoveryTest, GetImportModule_ProviderReturnsModule) {
    auto provider = std::make_unique<TestProvider>("guid-load", "load_alias", "my_module");
    ProviderRegistry::instance().registerProvider(std::move(provider));

    const ProtoList* chain = ctx->newList();
    ASSERT_NE(chain, nullptr);
    chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:load_alias"));
    ASSERT_NE(chain, nullptr);
    space.setResolutionChain(chain->asObject(ctx));

    const ProtoObject* result = space.getImportModule(ctx, "my_module", "exports");
    ASSERT_NE(result, PROTO_NONE);
    ASSERT_NE(result, nullptr);

    const ProtoString* key = ProtoString::fromUTF8(ctx, "exports");
    ASSERT_NE(key, nullptr);
    const ProtoObject* exports = result->getAttribute(ctx, key);
    ASSERT_NE(exports, PROTO_NONE);
    ASSERT_NE(exports, nullptr);
}

TEST_F(ModuleDiscoveryTest, GetImportModule_CacheHit) {
    auto provider = std::make_unique<TestProvider>("guid-cache", "cache_alias", "cached_mod");
    ProviderRegistry::instance().registerProvider(std::move(provider));

    const ProtoList* chain = ctx->newList();
    ASSERT_NE(chain, nullptr);
    chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:cache_alias"));
    space.setResolutionChain(chain->asObject(ctx));

    const ProtoObject* first = space.getImportModule(ctx, "cached_mod", "exports");
    ASSERT_NE(first, PROTO_NONE);
    const ProtoObject* second = space.getImportModule(ctx, "cached_mod", "exports");
    ASSERT_NE(second, PROTO_NONE);

    const ProtoString* key = ProtoString::fromUTF8(ctx, "exports");
    const ProtoObject* exp1 = first->getAttribute(ctx, key);
    const ProtoObject* exp2 = second->getAttribute(ctx, key);
    ASSERT_EQ(exp1, exp2);
}

TEST_F(ModuleDiscoveryTest, ProtoString_ToUTF8String) {
    const ProtoString* s = ProtoString::fromUTF8(ctx, "hello");
    ASSERT_NE(s, nullptr);
    std::string out;
    s->toUTF8String(ctx, out);
    ASSERT_EQ(out, "hello");
}

// P3 D11: the cache probe moved INSIDE the resolution-chain loop, because a
// module's identity is provider + path + version and the provider is not known
// until an entry is selected.  That changes resolution in one observable way,
// and this is the test of it: a module already loaded from a LATER chain entry
// no longer shadows an EARLIER entry that can serve the same path.  Chain order
// is the user's stated precedence.
//
// Before P3, `sharedModuleCacheGet(path)` ran before the loop, so the first load
// won for every later resolution whatever the chain said.
//
// MUTATION THAT MUST TURN THIS RED: move the cache probe back above the chain
// loop and key it by the bare path.  The second import then returns the module
// the LATE provider loaded.
TEST_F(ModuleDiscoveryTest, AnEarlierChainEntryIsNotShadowedByAnEarlierLoad) {
    auto early = std::make_unique<TestProvider>("guid-p3-early", "p3_early", "p3_shadowed_mod");
    auto late  = std::make_unique<TestProvider>("guid-p3-late",  "p3_late",  "p3_shadowed_mod");
    ProviderRegistry::instance().registerProvider(std::move(early));
    ProviderRegistry::instance().registerProvider(std::move(late));

    const ProtoString* key = ProtoString::fromUTF8(ctx, "exports");
    ASSERT_NE(key, nullptr);

    // 1. Resolve with ONLY the late provider in the chain, so the late
    //    provider's module is the one in the cache.
    {
        const ProtoList* chain = ctx->newList();
        chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:p3_late"));
        space.setResolutionChain(chain->asObject(ctx));
        const ProtoObject* wrapper = space.getImportModule(ctx, "p3_shadowed_mod", "exports");
        ASSERT_NE(wrapper, PROTO_NONE);
        ASSERT_NE(wrapper, nullptr);
    }
    const ProtoObject* fromLate = nullptr;
    {
        const ProtoList* chain = ctx->newList();
        chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:p3_late"));
        space.setResolutionChain(chain->asObject(ctx));
        const ProtoObject* wrapper = space.getImportModule(ctx, "p3_shadowed_mod", "exports");
        fromLate = wrapper->getAttribute(ctx, key);
        ASSERT_NE(fromLate, PROTO_NONE);
    }

    // 2. Now put the early provider FIRST.  Under a provider-qualified identity
    //    the early entry is probed with ITS OWN key, misses, and loads.
    {
        const ProtoList* chain = ctx->newList();
        chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:p3_early"));
        chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:p3_late"));
        space.setResolutionChain(chain->asObject(ctx));
        const ProtoObject* wrapper = space.getImportModule(ctx, "p3_shadowed_mod", "exports");
        ASSERT_NE(wrapper, PROTO_NONE);
        ASSERT_NE(wrapper, nullptr);
        const ProtoObject* fromEarly = wrapper->getAttribute(ctx, key);
        ASSERT_NE(fromEarly, PROTO_NONE);
        EXPECT_NE(fromEarly, fromLate)
            << "the module the later chain entry loaded first shadowed the earlier "
               "entry: the cache is still keyed by path alone";
    }

    // 3. And the early entry's module is itself cached under its own identity.
    {
        const ProtoList* chain = ctx->newList();
        chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:p3_early"));
        space.setResolutionChain(chain->asObject(ctx));
        const ProtoObject* a = space.getImportModule(ctx, "p3_shadowed_mod", "exports")
                                   ->getAttribute(ctx, key);
        const ProtoObject* b = space.getImportModule(ctx, "p3_shadowed_mod", "exports")
                                   ->getAttribute(ctx, key);
        EXPECT_EQ(a, b) << "the provider-qualified key does not cache at all";
    }
}
