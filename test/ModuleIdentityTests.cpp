// ModuleIdentityTests.cpp — P3 D11: a module's identity is provider + path +
// version, ruled by the maintainer on 2026-09-24.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

// The three components are all part of the key.  Path alone aliased silently.
TEST(ModuleIdentity, ProviderIsPartOfTheIdentity) {
    const ModuleIdentity fromST   = ModuleIdentity::unversioned("guid-st",    "counter_lib");
    const ModuleIdentity fromDisk = ModuleIdentity::unversioned("guid-fs-cwd", "counter_lib");
    EXPECT_NE(fromST.asKey(), fromDisk.asKey())
        << "provider:st/counter_lib and a local counter_lib must be two modules";
    EXPECT_FALSE(fromST == fromDisk);
}

TEST(ModuleIdentity, VersionIsPartOfTheIdentity) {
    const ModuleIdentity v1 = ModuleIdentity("g", "p", "1.0.0");
    const ModuleIdentity v2 = ModuleIdentity("g", "p", "2.0.0");
    EXPECT_NE(v1.asKey(), v2.asKey())
        << "two coexisting versions are two modules for the life of the process";
}

// The forward-compatibility rule.  An unversioned module's key must be
// byte-identical before and after manifests exist, or introducing versions would
// silently re-alias every module loaded today.
TEST(ModuleIdentity, TheUnversionedKeyIsFrozen) {
    const ModuleIdentity u = ModuleIdentity::unversioned("g", "p");
    EXPECT_EQ(u.getVersion(), "");
    EXPECT_EQ(u.asKey(), std::string("g") + '\x1F' + "p" + '\x1F')
        << "the unversioned key's byte sequence is frozen; changing it re-aliases "
           "every module already loaded in the field";
    // "" is reserved and is NOT a synonym for any declared version.
    EXPECT_NE(u.asKey(), ModuleIdentity("g", "p", "0.0.0").asKey());
    EXPECT_NE(u.asKey(), ModuleIdentity("g", "p", "latest").asKey());
    EXPECT_NE(u.asKey(), ModuleIdentity("g", "p", "unversioned").asKey());
}

// The separator cannot be produced by any component, so no two distinct triples
// can render to one key.
TEST(ModuleIdentity, TheSeparatorCannotBeForged) {
    // A path containing a '/' or a ':' must not be confusable with a different
    // provider/path split.
    EXPECT_NE(ModuleIdentity::unversioned("a", "b/c").asKey(),
              ModuleIdentity::unversioned("a/b", "c").asKey());
    EXPECT_NE(ModuleIdentity::unversioned("a", "b:c").asKey(),
              ModuleIdentity::unversioned("a:b", "c").asKey());
}

// The components are readable back exactly as given.
TEST(ModuleIdentity, ComponentsRoundTrip) {
    const ModuleIdentity id = ModuleIdentity("the-guid", "pkg/mod", "3.1.4");
    EXPECT_EQ(id.getProviderGUID(), "the-guid");
    EXPECT_EQ(id.getLogicalPath(),  "pkg/mod");
    EXPECT_EQ(id.getVersion(),      "3.1.4");
    EXPECT_EQ(id.asKey(), std::string("the-guid") + '\x1F' + "pkg/mod" + '\x1F' + "3.1.4");
    EXPECT_TRUE(id == ModuleIdentity("the-guid", "pkg/mod", "3.1.4"));
}
