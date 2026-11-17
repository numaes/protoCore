// ObjectShapeAttributesTests.cpp — Tests for ProtoShape, 4x6^d value trees,
// per-thread shape cache, unrolled depth accessors, and ProtoObject::newClone.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <string>
#include <vector>

using namespace proto;

namespace {

class ObjectShapeAttributesTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* ctx = nullptr;

    void SetUp() override {
        space = new ProtoSpace();
        ctx = space->rootContext;
    }

    void TearDown() override {
        delete space;
    }

    const ProtoString* sym(const std::string& s) {
        return ProtoString::createSymbol(ctx, s.c_str());
    }

    proto_ulong cellsAllocatedBy(const std::function<void(ProtoContext*)>& fn) {
        ProtoContext sub(space, ctx, nullptr, nullptr, nullptr, nullptr);
        const proto_ulong before = sub.allocatedCellsCount;
        fn(&sub);
        return sub.allocatedCellsCount - before;
    }
};

}  // namespace

TEST_F(ObjectShapeAttributesTest, EmptyObjectAllocatesSingleCellWithNullShape) {
    const ProtoObject* obj = nullptr;
    const proto_ulong cells = cellsAllocatedBy([&](ProtoContext* c) {
        obj = c->newObject(false);
    });
    EXPECT_EQ(cells, 1u);
    const auto* oc = toImpl<const ProtoObjectCell>(obj);
    EXPECT_EQ(oc->shape, nullptr);
    EXPECT_FALSE(oc->isMutableCell());
}

TEST_F(ObjectShapeAttributesTest, ShapePacksFromLastSlotToFirstAndChainsAtFour) {
    const ProtoObject* obj = ctx->newObject(false);
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 9; ++i) {
        keys.push_back(sym("attr_" + std::to_string(i)));
        obj = obj->setAttribute(ctx, keys[i], ctx->fromInteger(i * 10));

        const auto* oc = toImpl<const ProtoObjectCell>(obj);
        ASSERT_NE(oc->shape, nullptr);
        EXPECT_EQ(oc->shape->count, static_cast<proto_ulong>(i + 1));

        const unsigned usedInHead = (i % 4) + 1;
        for (unsigned slot = 0; slot < 4; ++slot) {
            if (slot < 4 - usedInHead) {
                EXPECT_EQ(oc->shape->names[slot], nullptr) << "i=" << i << " slot=" << slot;
            } else {
                const int keyIdx = i - static_cast<int>(slot - (4 - usedInHead));
                EXPECT_EQ(oc->shape->names[slot], keys[keyIdx]) << "i=" << i << " slot=" << slot;
            }
        }
    }

    const auto* oc9 = toImpl<const ProtoObjectCell>(obj);
    ASSERT_NE(oc9->shape, nullptr);
    EXPECT_EQ(oc9->shape->count, 9u);
    ASSERT_NE(oc9->shape->next, nullptr);
    EXPECT_EQ(oc9->shape->next->count, 8u);
    ASSERT_NE(oc9->shape->next->next, nullptr);
    EXPECT_EQ(oc9->shape->next->next->count, 4u);
    EXPECT_EQ(oc9->shape->next->next->next, nullptr);
}

TEST_F(ObjectShapeAttributesTest, ExactCellCountsAcrossIndirectionLevels) {
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 160; ++i) {
        keys.push_back(sym("k" + std::to_string(i)));
    }

    // Build an object with 4 attributes (d = 0, inline in ProtoObjectCell)
    const ProtoObject* obj4 = ctx->newObject(false);
    for (int i = 0; i < 4; ++i) {
        obj4 = obj4->setAttribute(ctx, keys[i], ctx->fromInteger(i));
    }
    const ProtoShape* shape4 = toImpl<const ProtoObjectCell>(obj4)->shape;

    // Overwriting any of the 4 inline attributes allocates ONLY 1 cell (the new ProtoObjectCell)
    // and keeps the exact same ProtoShape*!
    for (int i = 0; i < 4; ++i) {
        const ProtoObject* updated = nullptr;
        const proto_ulong cells = cellsAllocatedBy([&](ProtoContext* c) {
            updated = obj4->setAttribute(c, keys[i], c->fromInteger(100 + i));
        });
        EXPECT_EQ(cells, 1u) << "overwrite at d=0 index " << i;
        EXPECT_EQ(toImpl<const ProtoObjectCell>(updated)->shape, shape4);
        EXPECT_EQ(updated->getAttribute(ctx, keys[i])->asLong(ctx), 100 + i);
    }

    // Build an object with 16 attributes (d = 1, 5 <= count <= 24)
    const ProtoObject* obj16 = obj4;
    for (int i = 4; i < 16; ++i) {
        obj16 = obj16->setAttribute(ctx, keys[i], ctx->fromInteger(i));
    }
    const ProtoShape* shape16 = toImpl<const ProtoObjectCell>(obj16)->shape;

    // Overwriting any of the 16 attributes allocates exactly 2 cells (1 Leaf + 1 ProtoObjectCell)
    for (int i : {0, 3, 4, 7, 12, 15}) {
        const ProtoObject* updated = nullptr;
        const proto_ulong cells = cellsAllocatedBy([&](ProtoContext* c) {
            updated = obj16->setAttribute(c, keys[i], c->fromInteger(200 + i));
        });
        EXPECT_EQ(cells, 2u) << "overwrite at d=1 index " << i;
        EXPECT_EQ(toImpl<const ProtoObjectCell>(updated)->shape, shape16);
        EXPECT_EQ(updated->getAttribute(ctx, keys[i])->asLong(ctx), 200 + i);
    }

    // Build an object with 100 attributes (d = 2, 25 <= count <= 144)
    const ProtoObject* obj100 = obj16;
    for (int i = 16; i < 100; ++i) {
        obj100 = obj100->setAttribute(ctx, keys[i], ctx->fromInteger(i));
    }
    const ProtoShape* shape100 = toImpl<const ProtoObjectCell>(obj100)->shape;

    // Overwriting any of the 100 attributes allocates exactly 3 cells (1 Leaf + 1 Branch + 1 ProtoObjectCell)
    for (int i : {0, 5, 24, 35, 36, 71, 99}) {
        const ProtoObject* updated = nullptr;
        const proto_ulong cells = cellsAllocatedBy([&](ProtoContext* c) {
            updated = obj100->setAttribute(c, keys[i], c->fromInteger(300 + i));
        });
        EXPECT_EQ(cells, 3u) << "overwrite at d=2 index " << i;
        EXPECT_EQ(toImpl<const ProtoObjectCell>(updated)->shape, shape100);
        EXPECT_EQ(updated->getAttribute(ctx, keys[i])->asLong(ctx), 300 + i);
    }

    // Build an object with 160 attributes (d = 3, 145 <= count <= 864)
    const ProtoObject* obj160 = obj100;
    for (int i = 100; i < 160; ++i) {
        obj160 = obj160->setAttribute(ctx, keys[i], ctx->fromInteger(i));
    }
    const ProtoShape* shape160 = toImpl<const ProtoObjectCell>(obj160)->shape;

    // Overwriting any of the 160 attributes allocates exactly 4 cells (1 Leaf + 2 Branches + 1 ProtoObjectCell)
    for (int i : {0, 24, 143, 144, 159}) {
        const ProtoObject* updated = nullptr;
        const proto_ulong cells = cellsAllocatedBy([&](ProtoContext* c) {
            updated = obj160->setAttribute(c, keys[i], c->fromInteger(400 + i));
        });
        EXPECT_EQ(cells, 4u) << "overwrite at d=3 index " << i;
        EXPECT_EQ(toImpl<const ProtoObjectCell>(updated)->shape, shape160);
        EXPECT_EQ(updated->getAttribute(ctx, keys[i])->asLong(ctx), 400 + i);
    }
}

TEST_F(ObjectShapeAttributesTest, DeepIndirectionBeyond864Attributes) {
    // Verify transition across 864 -> 865 (d = 3 -> d = 4) and up to 950 attributes
    constexpr int N = 950;
    std::vector<const ProtoString*> keys;
    keys.reserve(N);
    const ProtoObject* obj = ctx->newObject(false);
    for (int i = 0; i < N; ++i) {
        keys.push_back(sym("deep_" + std::to_string(i)));
        obj = obj->setAttribute(ctx, keys[i], ctx->fromInteger(i * 3));
    }

    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(obj->getAttribute(ctx, keys[i])->asLong(ctx), i * 3) << "index " << i;
    }

    // Overwrite at boundary indices (0, 24, 144, 864, 949)
    for (int idx : {0, 4, 24, 144, 863, 864, 949}) {
        obj = obj->setAttribute(ctx, keys[idx], ctx->fromInteger(-idx - 1));
    }
    for (int idx : {0, 4, 24, 144, 863, 864, 949}) {
        EXPECT_EQ(obj->getAttribute(ctx, keys[idx])->asLong(ctx), -idx - 1);
    }
}

TEST_F(ObjectShapeAttributesTest, NewCloneSharesShapeAcrossTreeNodesAndIsolatesValues) {
    const ProtoString* kVal = sym("val");
    const ProtoString* kLeft = sym("left");
    const ProtoString* kRight = sym("right");

    // Prototype / template node with 3 attributes
    const ProtoObject* protoNode = ctx->newObject(false)
        ->setAttribute(ctx, kVal, ctx->fromInteger(0))
        ->setAttribute(ctx, kLeft, PROTO_NONE)
        ->setAttribute(ctx, kRight, PROTO_NONE);

    const ProtoShape* baseShape = toImpl<const ProtoObjectCell>(protoNode)->shape;
    ASSERT_NE(baseShape, nullptr);
    EXPECT_EQ(baseShape->count, 3u);

    // Create 100 tree nodes via newClone and update their values
    std::vector<const ProtoObject*> nodes(100);
    for (int i = 0; i < 100; ++i) {
        const ProtoObject* n = nullptr;
        const proto_ulong cloneCells = cellsAllocatedBy([&](ProtoContext* c) {
            n = protoNode->newClone(c, false);
        });
        EXPECT_EQ(cloneCells, 1u);

        n = n->setAttribute(ctx, kVal, ctx->fromInteger(i));
        if (i > 0) {
            n = n->setAttribute(ctx, kLeft, nodes[(i - 1) / 2]);
        }
        nodes[i] = n;
        // Every node in the tree still shares the exact same ProtoShape*!
        EXPECT_EQ(toImpl<const ProtoObjectCell>(n)->shape, baseShape);
    }

    // Verify all values are independent and intact
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(nodes[i]->getAttribute(ctx, kVal)->asLong(ctx), i);
        if (i > 0) {
            EXPECT_EQ(nodes[i]->getAttribute(ctx, kLeft), nodes[(i - 1) / 2]);
        } else {
            EXPECT_EQ(nodes[i]->getAttribute(ctx, kLeft), PROTO_NONE);
        }
    }

    // Extending one cloned node with an extra attribute shares the base shape as its tail
    const ProtoString* kExtra = sym("extra");
    const ProtoObject* extended = nodes[50]->setAttribute(ctx, kExtra, ctx->fromInteger(999));
    const ProtoShape* extShape = toImpl<const ProtoObjectCell>(extended)->shape;
    ASSERT_NE(extShape, nullptr);
    EXPECT_EQ(extShape->count, 4u);
    EXPECT_EQ(extended->getAttribute(ctx, kExtra)->asLong(ctx), 999);
    EXPECT_EQ(extended->getAttribute(ctx, kVal)->asLong(ctx), 50);
    EXPECT_EQ(nodes[50]->hasOwnAttribute(ctx, kExtra), PROTO_FALSE);
}

TEST_F(ObjectShapeAttributesTest, RemoveAndReAddReusesSameOffsetAndShape) {
    const ProtoString* k0 = sym("a");
    const ProtoString* k1 = sym("b");
    const ProtoString* k2 = sym("c");

    const ProtoObject* obj = ctx->newObject(false)
        ->setAttribute(ctx, k0, ctx->fromInteger(10))
        ->setAttribute(ctx, k1, ctx->fromInteger(20))
        ->setAttribute(ctx, k2, ctx->fromInteger(30));
    const ProtoShape* shapeBefore = toImpl<const ProtoObjectCell>(obj)->shape;

    const ProtoObject* removed = obj->removeAttribute(ctx, k1);
    EXPECT_EQ(toImpl<const ProtoObjectCell>(removed)->shape, shapeBefore);
    EXPECT_EQ(removed->hasOwnAttribute(ctx, k1), PROTO_FALSE);
    EXPECT_EQ(removed->getAttribute(ctx, k1), PROTO_NONE);
    EXPECT_EQ(removed->getAttribute(ctx, k0)->asLong(ctx), 10);
    EXPECT_EQ(removed->getAttribute(ctx, k2)->asLong(ctx), 30);

    // Re-adding k1 reuses its existing offset in shapeBefore without growing the shape!
    const ProtoObject* readded = removed->setAttribute(ctx, k1, ctx->fromInteger(200));
    EXPECT_EQ(toImpl<const ProtoObjectCell>(readded)->shape, shapeBefore);
    EXPECT_EQ(toImpl<const ProtoObjectCell>(readded)->shape->count, 3u);
    EXPECT_EQ(readded->hasOwnAttribute(ctx, k1), PROTO_TRUE);
    EXPECT_EQ(readded->getAttribute(ctx, k1)->asLong(ctx), 200);
}

TEST_F(ObjectShapeAttributesTest, MutableNewClonePreservesInitialStateAndMutatesIndependently) {
    const ProtoString* kx = sym("x");
    const ProtoString* ky = sym("y");

    const ProtoObject* base = ctx->newObject(false)
        ->setAttribute(ctx, kx, ctx->fromInteger(7))
        ->setAttribute(ctx, ky, ctx->fromInteger(8));

    const ProtoObject* mutClone = base->newClone(ctx, true);
    EXPECT_EQ(mutClone->getAttribute(ctx, kx)->asLong(ctx), 7);
    EXPECT_EQ(mutClone->getAttribute(ctx, ky)->asLong(ctx), 8);

    // Mutate mutClone in place
    EXPECT_EQ(mutClone->setAttribute(ctx, kx, ctx->fromInteger(77)), mutClone);
    EXPECT_EQ(mutClone->getAttribute(ctx, kx)->asLong(ctx), 77);
    EXPECT_EQ(base->getAttribute(ctx, kx)->asLong(ctx), 7);

    // Clone mutable into immutable via newClone(ctx, false)
    const ProtoObject* frozen = mutClone->newClone(ctx, false);
    EXPECT_EQ(frozen->getAttribute(ctx, kx)->asLong(ctx), 77);
    EXPECT_EQ(frozen->getAttribute(ctx, ky)->asLong(ctx), 8);
    EXPECT_EQ(toImpl<const ProtoObjectCell>(frozen)->shape,
              toImpl<const ProtoObjectCell>(base)->shape);
}

TEST_F(ObjectShapeAttributesTest, GcStressAcrossAllIndirectionLevels) {
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 40; ++i) {
        keys.push_back(sym("gc_k" + std::to_string(i)));
    }

    const ProtoObject* liveMut = ctx->newObject(true);
    for (int i = 0; i < 40; ++i) {
        liveMut->setAttribute(ctx, keys[i], ctx->fromUTF8String(("val_" + std::to_string(i)).c_str()));
    }

    const uint64_t startCycles = space->getGCCycleCount();
    space->setHeapLimits(0, space->heapSize + 30000);
    for (int round = 0; round < 500 && space->getGCCycleCount() - startCycles < 5; ++round) {
        ProtoContext sub(space, ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* temp = liveMut->newClone(&sub, false);
        temp = temp->setAttribute(&sub, keys[round % 40], sub.fromInteger(round));
        for (int g = 0; g < 2000; ++g) (void) sub.newObject(false);
        liveMut->setAttribute(&sub, keys[round % 40],
                              sub.fromUTF8String(("updated_" + std::to_string(round % 40)).c_str()));
        sub.safepoint();
    }
    space->setHeapLimits(0, 0);
    EXPECT_GT(space->getGCCycleCount(), startCycles);

    for (int i = 0; i < 40; ++i) {
        const ProtoObject* v = liveMut->getAttribute(ctx, keys[i]);
        ASSERT_NE(v, nullptr);
        EXPECT_TRUE(v->isString(ctx)) << "i=" << i;
    }
}

