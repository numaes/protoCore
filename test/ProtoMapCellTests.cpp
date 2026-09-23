// ProtoMapCellTests.cpp — the ProtoMap cells: one pointer tag for both
// forms, their own CellTypes, and processReferences reporting the key only
// when it is a cell pointer (embedded keys are never reported).

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

using namespace proto;

namespace {
    void record(ProtoContext*, void* self, const Cell* cell) {
        static_cast<std::vector<const Cell*>*>(self)->push_back(cell);
    }
    bool contains(const std::vector<const Cell*>& v, const Cell* c) {
        return std::find(v.begin(), v.end(), c) != v.end();
    }
    const ProtoObject* cellObject(ProtoContext* c, long n) {
        return c->newList()->appendLast(c, c->fromInteger(n))->asObject(c);
    }
    // The Small constructor requires its keys in ascending key-word order;
    // sort the pairs by the key word so the test never relies on the
    // numeric values of particular encodings or heap addresses.
    void sortByKeyWord(const ProtoObject** ks, const ProtoObject** vs, unsigned n) {
        for (unsigned i = 1; i < n; ++i)
            for (unsigned j = i; j > 0 && reinterpret_cast<uintptr_t>(ks[j - 1]) > reinterpret_cast<uintptr_t>(ks[j]); --j) {
                std::swap(ks[j - 1], ks[j]);
                std::swap(vs[j - 1], vs[j]);
            }
    }
}

TEST(MapCells, BothFormsShareOneTagAndHaveTheirOwnCellType) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    EXPECT_EQ(POINTER_TAG_MAP, 27);

    auto* node = new(c) ProtoMapImplementation(c, nullptr, nullptr, nullptr, nullptr, true);
    auto* small = new(c) ProtoMapSmallImplementation(c);
    ProtoObjectPointer a{}, b{};
    a.oid = node->implAsObject(c);
    b.oid = small->implAsObject(c);
    EXPECT_EQ(a.op.pointer_tag, static_cast<unsigned long>(POINTER_TAG_MAP));
    EXPECT_EQ(b.op.pointer_tag, static_cast<unsigned long>(POINTER_TAG_MAP));
    EXPECT_EQ(node->getType(), CellType::Map);
    EXPECT_EQ(small->getType(), CellType::MapSmall);
    EXPECT_LE(sizeof(ProtoMapImplementation), 64u);
    EXPECT_LE(sizeof(ProtoMapSmallImplementation), 64u);
}

TEST(MapCells, NodeReportsCellKeyCellValueAndChildren) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoObject* k1 = cellObject(c, 1);
    const ProtoObject* v1 = cellObject(c, 2);
    auto* leaf = new(c) ProtoMapImplementation(c, k1, v1, nullptr, nullptr, false);

    std::vector<const Cell*> seen;
    leaf->processReferences(c, &seen, record);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_TRUE(contains(seen, ProtoObject::asCellPointer(k1)));
    EXPECT_TRUE(contains(seen, ProtoObject::asCellPointer(v1)));

    const ProtoObject* k2 = cellObject(c, 3);
    auto* parent = new(c) ProtoMapImplementation(c, k2, c->fromInteger(5), leaf, nullptr, false);
    seen.clear();
    parent->processReferences(c, &seen, record);
    ASSERT_EQ(seen.size(), 2u);                      // embedded value is not reported
    EXPECT_TRUE(contains(seen, ProtoObject::asCellPointer(k2)));
    EXPECT_TRUE(contains(seen, leaf));
    for (const Cell* s : seen) EXPECT_EQ(reinterpret_cast<uintptr_t>(s) & 0x3F, 0u);
}

TEST(MapCells, EmbeddedKeysAreNeverReported) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoObject* embedded[] = {
        c->fromInteger(7), c->fromInteger(-9), PROTO_TRUE, PROTO_FALSE,
        c->fromUnicodeChar(0x41), PROTO_NONE, c->fromUTF8String("ab")
    };
    for (const ProtoObject* k : embedded) {
        ASSERT_EQ(ProtoObject::asCellPointer(k), nullptr);
        auto* node = new(c) ProtoMapImplementation(c, k, c->fromInteger(1), nullptr, nullptr, false);
        std::vector<const Cell*> seen;
        node->processReferences(c, &seen, record);
        EXPECT_TRUE(seen.empty());
    }

    const ProtoObject* ks[3] = {embedded[0], embedded[2], embedded[5]};
    const ProtoObject* vsEmbedded[3] = {c->fromInteger(1), c->fromInteger(2), c->fromInteger(3)};
    sortByKeyWord(ks, vsEmbedded, 3);
    auto* smallEmbedded = new(c) ProtoMapSmallImplementation(c, 3, ks, vsEmbedded);
    std::vector<const Cell*> seen;
    smallEmbedded->processReferences(c, &seen, record);
    EXPECT_TRUE(seen.empty());

    const ProtoObject* cellValue = cellObject(c, 4);
    const ProtoObject* cellKey = cellObject(c, 5);
    const ProtoObject* ks2[2] = {embedded[0], cellKey};
    const ProtoObject* vs2[2] = {cellValue, c->fromInteger(6)};
    sortByKeyWord(ks2, vs2, 2);
    auto* smallMixed = new(c) ProtoMapSmallImplementation(c, 2, ks2, vs2);
    seen.clear();
    smallMixed->processReferences(c, &seen, record);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_TRUE(contains(seen, ProtoObject::asCellPointer(cellValue)));
    EXPECT_TRUE(contains(seen, ProtoObject::asCellPointer(cellKey)));
}

TEST(MapCells, NodeWithBothChildrenReportsExactlyItsFourReferences) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoObject* kl = cellObject(c, 10);
    const ProtoObject* kr = cellObject(c, 11);
    auto* left = new(c) ProtoMapImplementation(c, kl, c->fromInteger(1), nullptr, nullptr, false);
    auto* right = new(c) ProtoMapImplementation(c, kr, c->fromInteger(2), nullptr, nullptr, false);
    const ProtoObject* key = cellObject(c, 12);
    const ProtoObject* value = cellObject(c, 13);
    auto* node = new(c) ProtoMapImplementation(c, key, value, left, right, false);

    std::vector<const Cell*> seen;
    node->processReferences(c, &seen, record);
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_TRUE(contains(seen, ProtoObject::asCellPointer(key)));
    EXPECT_TRUE(contains(seen, ProtoObject::asCellPointer(value)));
    EXPECT_TRUE(contains(seen, left));
    EXPECT_TRUE(contains(seen, right));
}
