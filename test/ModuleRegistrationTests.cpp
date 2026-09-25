// ModuleRegistrationTests.cpp — P3 D13: an embedder that loads a module itself
// can publish it under the ruled identity and have it rooted in ITS OWN space.
//
// Before P3, a prefixed cross-runtime import (protoScala's Session::loadForeign)
// called provider->tryLoad directly, so it reached neither SharedModuleCache nor
// any moduleRoots, and its only anchor was inside the PROVIDING runtime.
// Destroying that runtime while an importer still held its values dropped the
// only anchor; the tests survived only because construction order happened to
// make the provider destroyed last, and nothing enforced that.
//
// forceCycles / ASSERT_CYCLES_DID_REAL_WORK and the module-shaped helpers are
// duplicated here on purpose: no test file in this suite may depend on another's
// link order.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <cstdio>
#include <string>

using namespace proto;

namespace {

constexpr int kHeadroomCells   = 40000;
constexpr int kGarbagePerBatch = 5000;

struct CycleReport {
    uint64_t      cycles;
    unsigned long reclaimed;
    long          created;
};

// See test/GlobalInterningTests.cpp for the two traps this shape avoids:
// forcing a cycle is not the same as submitting the young generation, and a
// reclamation assertion must be consistent with the garbage created.
CycleReport forceCycles(ProtoSpace& space, ProtoContext* parent, uint64_t minCycles) {
    const uint64_t start = space.getGCCycleCount();
    long created = 0;
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + kHeadroomCells);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        ProtoContext garbage(&space, parent, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kGarbagePerBatch; ++i) {
            (void) garbage.newObject(false);
            ++created;
            if ((i & 1023) == 0) garbage.safepoint();
        }
        garbage.safepoint();
    }
    space.setHeapLimits(0, 0);
    return CycleReport{ space.getGCCycleCount() - start,
                        space.reclaimedLastCycle.load(std::memory_order_relaxed),
                        created };
}

// A stand-in for a loaded module: an object whose attribute "moduleVariable"
// holds a freshly allocated, verifiable structure.  This is the shape that
// matters — a module anchors its contents through its variables.
const ProtoObject* buildModuleLike(ProtoContext* c, long tag) {
    ProtoContext::CriticalSection cs(c);
    const ProtoObject* contents =
        c->newList()->appendLast(c, c->fromInteger(tag))
                    ->appendLast(c, c->fromInteger(~tag))->asObject(c);
    const ProtoString* key = ProtoString::createSymbol(c, "moduleVariable");
    return c->newObject(false)->setAttribute(c, key, contents);
}

bool moduleIntact(ProtoContext* c, const ProtoObject* module, long tag) {
    const ProtoString* key = ProtoString::createSymbol(c, "moduleVariable");
    const ProtoObject* contents = module->getAttribute(c, key);
    if (!contents || contents == PROTO_NONE) return false;
    const ProtoList* l = contents->asList(c);
    return l && l->getSize(c) == 2 &&
           l->getAt(c, 0)->asLong(c) == tag &&
           l->getAt(c, 1)->asLong(c) == ~tag;
}

std::string uniqueIdentityPath(const char* stem) {
    static std::atomic<unsigned long> counter{0};
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string out = "p3_";
    out += info ? info->name() : "no_test";
    out += '_';
    out += stem;
    out += '_';
    out += std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return out;
}

}  // namespace

#define ASSERT_CYCLES_DID_REAL_WORK(rep, minCycles)                              \
    do {                                                                         \
        std::fprintf(stderr, "[gc] cycles=%lu reclaimed=%lu created=%ld\n",      \
                     (unsigned long)(rep).cycles, (rep).reclaimed, (rep).created); \
        ASSERT_GE((rep).cycles, (uint64_t)(minCycles))                           \
            << "no collection ran; the test proves nothing";                     \
        ASSERT_GT((rep).reclaimed, (unsigned long)((rep).created / 10))          \
            << "the cycles reclaimed " << (rep).reclaimed << " cells against "   \
            << (rep).created << " created: the young generation was never "      \
            << "submitted, so this test would pass with the GC disabled";        \
    } while (0)

// Two importers of the same identity share one module.
TEST(ModuleRegistration, TheSameIdentityYieldsTheSameModule) {
    ProtoSpace a, b;
    ProtoContext ca(&a, a.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoContext cb(&b, b.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ModuleIdentity id =
        ModuleIdentity::unversioned("guid-test", uniqueIdentityPath("shared_lib"));

    const ProtoObject* first  = a.registerModule(id, ca.newObject(false));
    const ProtoObject* second = b.registerModule(id, cb.newObject(false));
    EXPECT_EQ(first, second) << "a module is loaded once for the process";
    EXPECT_EQ(ProtoSpace::findModule(id), first);
}

// An identity that was never registered is not found.
TEST(ModuleRegistration, AnUnregisteredIdentityIsNotFound) {
    const ModuleIdentity id =
        ModuleIdentity::unversioned("guid-test", uniqueIdentityPath("never_registered"));
    EXPECT_EQ(ProtoSpace::findModule(id), nullptr);
}

// The registering space becomes the module's owner, and destroying the space
// that PROVIDED the module no longer drops it.  The module is built in the
// importer's heap, exactly as buildCallerFacade builds its facade in the
// caller's context, and registered by the importer.
//
// MUTATION THAT MUST TURN THIS RED: make registerModule insert into the cache
// without calling addModuleRoot.  The module is then found by identity and
// collected anyway — the "never freed is not a root" distinction again.
TEST(ModuleRegistration, ARegisteredModuleIsRootedInTheRegisteringSpace) {
    ProtoSpace importer;
    ProtoContext live(&importer, importer.rootContext, nullptr, nullptr, nullptr, nullptr);

    const ModuleIdentity id =
        ModuleIdentity::unversioned("guid-provider", uniqueIdentityPath("rooted_lib"));
    const ProtoObject* module = nullptr;
    {
        // `provider` plays the providing runtime.  It is constructed second and
        // destroyed FIRST, which is the order the family's tests never exercise
        // and the one that used to drop the only anchor.
        ProtoSpace provider;
        ProtoContext loader(&importer, &live, nullptr, nullptr, nullptr, nullptr);
        module = importer.registerModule(id, buildModuleLike(&loader, 23));
        ASSERT_NE(module, nullptr);
        ASSERT_NE(module, PROTO_NONE);
        loader.safepoint();
    }   // the providing space and the loading context are both gone

    const CycleReport rep = forceCycles(importer, &live, 3);
    ASSERT_CYCLES_DID_REAL_WORK(rep, 3);

    EXPECT_TRUE(moduleIntact(&live, module, 23))
        << "registerModule published the module but did not root it";
    EXPECT_EQ(ProtoSpace::findModule(id), module);
}
