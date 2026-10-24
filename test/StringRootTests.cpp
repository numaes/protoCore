// StringRootTests.cpp — a heap string is its rope root.
//
// Since 3.2.0 a heap string handle (POINTER_TAG_STRING, or POINTER_TAG_SYMBOL)
// points directly at its rope root, a StringLeafNode or a StringInternalNode,
// with no header cell: a string that fits in one leaf is one Cell.  The handle
// keeps tag 6 / 22 (isStringTagFast is inlined in embedders), so
// heapStringRoot tells the two node types apart by `node_kind`, which both
// declare as their first data member, at STRING_NODE_KIND_OFFSET.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <string>

using namespace proto;

namespace {

const Cell* cellOf(const ProtoObject* o) {
    return reinterpret_cast<const Cell*>(
        reinterpret_cast<uintptr_t>(o) & ~static_cast<uintptr_t>(0x3F));
}

std::string ascii(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s += static_cast<char>('a' + i % 26);
    return s;
}

// Cells the current context allocated while building fromUTF8(src).
proto_ulong cellsToBuild(ProtoContext* ctx, const std::string& src) {
    const proto_ulong before = ctx->allocatedCellsCount;
    (void) ProtoString::fromUTF8(ctx, src.c_str());
    return ctx->allocatedCellsCount - before;
}

}  // namespace

// heapStringRoot reads node_kind at a fixed offset of a node of either type.
TEST(StringRootLayout, NodeKindIsAtTheSameOffsetInBothNodeTypes) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const std::string shortSrc = ascii(20), longSrc = ascii(100);
    const ProtoObject* leafRoot = buildRopeFromUTF8(
        ctx, reinterpret_cast<const uint8_t*>(shortSrc.data()), shortSrc.size());
    const ProtoObject* internalRoot = buildRopeFromUTF8(
        ctx, reinterpret_cast<const uint8_t*>(longSrc.data()), longSrc.size());
    ASSERT_TRUE(StringLeafNode::isStringLeafNode(leafRoot));
    ASSERT_TRUE(StringInternalNode::isStringInternalNode(internalRoot));

    const auto* leaf = StringLeafNode::fromObject(leafRoot);
    const auto* node = StringInternalNode::fromObject(internalRoot);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&leaf->node_kind) -
              reinterpret_cast<const uint8_t*>(static_cast<const Cell*>(leaf)),
              STRING_NODE_KIND_OFFSET);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&node->node_kind) -
              reinterpret_cast<const uint8_t*>(static_cast<const Cell*>(node)),
              STRING_NODE_KIND_OFFSET);
    EXPECT_EQ(leaf->node_kind & STRING_NODE_KIND_MASK, STRING_NODE_LEAF);
    EXPECT_EQ(node->node_kind & STRING_NODE_KIND_MASK, STRING_NODE_INTERNAL);
}

// The handle of a one-leaf string is the leaf, and of a longer string the
// internal root; the round trip through heapStringRoot keeps the cell.
TEST(StringRootLayout, HandlePointsAtTheRopeRoot) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    for (size_t n : {7u, 20u, 32u, 33u, 100u, 5000u}) {
        const ProtoString* s = ProtoString::fromUTF8(ctx, ascii(n).c_str());
        const ProtoObject* o = reinterpret_cast<const ProtoObject*>(s);
        const ProtoObject* root = heapStringRoot(o);
        ASSERT_NE(root, nullptr) << n;
        EXPECT_EQ(cellOf(root), cellOf(o)) << n;
        EXPECT_EQ(s->asCell(ctx), cellOf(o)) << n;
        const CellType expected = n <= StringLeafNode::MAX_PAYLOAD
            ? CellType::StringLeafNode : CellType::StringInternalNode;
        EXPECT_EQ(cellOf(o)->getType(), expected) << n;
        EXPECT_EQ(heapStringFromRoot(root), o) << n;
        EXPECT_EQ(s->getSize(ctx), n) << n;
        EXPECT_EQ(s->toStdString(ctx), ascii(n)) << n;
    }
}

// A symbol is a perennial rope root carrying POINTER_TAG_SYMBOL.
TEST(StringRootLayout, SymbolIsARopeRootWithTheSymbolTag) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoString* sym = ProtoString::createSymbol(ctx, "a_symbol_of_24_bytes_xyz");
    ASSERT_TRUE(sym->isSymbol());
    const ProtoObject* root = heapStringRoot(reinterpret_cast<const ProtoObject*>(sym));
    ASSERT_TRUE(StringLeafNode::isStringLeafNode(root));
    EXPECT_EQ(heapStringFromRoot(root, /*symbol=*/true),
              reinterpret_cast<const ProtoObject*>(sym));
    EXPECT_EQ(sym->toStdString(ctx), "a_symbol_of_24_bytes_xyz");
}

// The cell count of a heap string is its rope: no header cell.
TEST(StringRootLayout, AHeapStringCostsOnlyItsRope) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    EXPECT_EQ(cellsToBuild(ctx, ascii(6)), 0u);    // inline
    EXPECT_EQ(cellsToBuild(ctx, ascii(7)), 1u);    // one leaf
    EXPECT_EQ(cellsToBuild(ctx, ascii(StringLeafNode::MAX_PAYLOAD)), 1u);
    EXPECT_EQ(cellsToBuild(ctx, ascii(StringLeafNode::MAX_PAYLOAD + 1u)), 3u);  // 2 leaves + 1 node
}
