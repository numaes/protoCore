// StringToStdStringTests.cpp -- ProtoString::toStdString / toUTF8String keep
// their exact output when the conversion copies leaf bytes in blocks.
//
// Up to 3.3.0 the conversion decoded every code point of the rope with the
// rope iterator and re-encoded it.  The oracle below is that algorithm,
// restricted to the node kinds a string holds (inline strings, leaves,
// internal nodes): per leaf, decode at each lead byte (reading the payload
// past byte_count exactly as the iterator did) and re-encode, dropping code
// points above U+10FFFF.  The tests compare the conversion with the oracle,
// byte for byte, on well-formed text (ASCII, 2/3/4-byte sequences, encoded
// surrogates, NUL), on every representation (inline, one leaf, many leaves,
// ropes joined with appendLast, slices), and on leaves built directly with
// malformed bytes, where a block copy would differ from the old output.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace proto;

namespace {

// ---- The 3.3.0 algorithm, as the oracle -------------------------------------

uint32_t oracleSeqLen(uint8_t lead) {
    if (lead < 0x80) return 1;
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    return 4;
}

uint32_t oracleDecode(const uint8_t* p) {
    const uint8_t lead = p[0];
    if (lead < 0x80) return lead;
    if (lead < 0xE0) return ((lead & 0x1Fu) << 6u) | (p[1] & 0x3Fu);
    if (lead < 0xF0) return ((lead & 0x0Fu) << 12u) | ((p[1] & 0x3Fu) << 6u) | (p[2] & 0x3Fu);
    return ((lead & 0x07u) << 18u) | ((p[1] & 0x3Fu) << 12u) | ((p[2] & 0x3Fu) << 6u) | (p[3] & 0x3Fu);
}

void oracleEncode(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800u) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x110000u) {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
}

void oracleLeaf(std::string& out, const uint8_t* payload, uint32_t count) {
    for (uint32_t pos = 0; pos < count; ) {
        oracleEncode(out, oracleDecode(payload + pos));
        pos += oracleSeqLen(payload[pos]);
    }
}

void oracleNode(std::string& out, const ProtoObject* node) {
    if (!node) return;
    if (StringLeafNode::isStringLeafNode(node)) {
        const StringLeafNode* leaf = StringLeafNode::fromObject(node);
        oracleLeaf(out, leaf->utf8_payload, leaf->byte_count);
        return;
    }
    ASSERT_TRUE(StringInternalNode::isStringInternalNode(node));
    const StringInternalNode* in = StringInternalNode::fromObject(node);
    oracleNode(out, in->left);
    oracleNode(out, in->right);
}

std::string oracle(const ProtoString* s) {
    const auto* o = reinterpret_cast<const ProtoObject*>(s);
    std::string out;
    if (isInlineString(o)) {
        // 3.3.0 materialized an inline string as a fresh leaf: the bytes
        // followed by a zero-filled payload.
        uint8_t payload[StringLeafNode::MAX_PAYLOAD + 4] = {};
        const proto_ulong n = inlineStringByteCount(o);
        for (proto_ulong i = 0; i < n; ++i) payload[i] = inlineStringByte(o, i);
        oracleLeaf(out, payload, static_cast<uint32_t>(n));
        return out;
    }
    oracleNode(out, heapStringRoot(o));
    return out;
}

// ---- Helpers ----------------------------------------------------------------

std::string encode(uint32_t cp) {
    std::string s;
    oracleEncode(s, cp);
    return s;
}

const ProtoString* fromBuffer(ProtoContext* ctx, const std::string& bytes) {
    uint8_t rem[4];
    uint8_t remCount = 0;
    return ProtoString::fromUTF8Buffer(ctx, reinterpret_cast<const uint8_t*>(bytes.data()),
                                       bytes.size(), nullptr, 0, rem, &remCount);
}

// A heap string over a leaf holding exactly `bytes` (at most 36), whatever
// they are: the constructors normalize malformed input, a leaf does not.
const ProtoString* rawLeafString(ProtoContext* ctx, const std::vector<uint8_t>& bytes) {
    const auto* leaf = new (ctx) StringLeafNode(ctx, bytes.data(),
                                                static_cast<uint8_t>(bytes.size()),
                                                static_cast<uint16_t>(bytes.size()));
    return reinterpret_cast<const ProtoString*>(heapStringFromRoot(leaf->asObject()));
}

const ProtoString* rawRope(ProtoContext* ctx, const std::vector<std::vector<uint8_t>>& leaves) {
    static const uint8_t none[1] = {0};
    const ProtoObject* root = nullptr;
    for (const auto& bytes : leaves) {
        const ProtoObject* leaf = (new (ctx) StringLeafNode(ctx, bytes.empty() ? none : bytes.data(),
                                                            static_cast<uint8_t>(bytes.size()),
                                                            static_cast<uint16_t>(bytes.size())))->asObject();
        root = root ? (new (ctx) StringInternalNode(ctx, root, leaf))->asObject() : leaf;
    }
    return reinterpret_cast<const ProtoString*>(heapStringFromRoot(root));
}

class StringToStdString : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    // toStdString and toUTF8String agree with the oracle; for well-formed
    // input they also return the bytes the string was built from.
    void check(const ProtoString* s, const std::string* built = nullptr) {
        const std::string expected = oracle(s);
        EXPECT_EQ(s->toStdString(ctx), expected);
        std::string viaOut = "stale content that must be replaced";
        s->toUTF8String(ctx, viaOut);
        EXPECT_EQ(viaOut, expected);
        if (built) EXPECT_EQ(expected, *built);
    }
};

}  // namespace

TEST_F(StringToStdString, EmptyAndInline) {
    for (const std::string& text : {std::string(""), std::string("a"), std::string("abcdef"),
                                    encode(0xE9), encode(0x20AC) + "ab", encode(0x1F600),
                                    encode(0xD800) + "x"}) {
        SCOPED_TRACE(text);
        const ProtoString* s = ProtoString::fromUTF8(ctx, text.c_str());
        ASSERT_LE(text.size(), static_cast<size_t>(INLINE_STRING_MAX_BYTES));
        EXPECT_TRUE(isInlineString(reinterpret_cast<const ProtoObject*>(s)));
        check(s, &text);
    }
}

TEST_F(StringToStdString, OneLeafManyLeavesAndRopes) {
    std::string ascii;
    for (int i = 0; i < 2000; ++i) {
        ascii += static_cast<char>('a' + i % 26);
        if (i == 6 || i == 35 || i == 36 || i == 37 || i == 71 || i == 72 || i == 1999) {
            SCOPED_TRACE(i);
            check(ProtoString::fromUTF8(ctx, ascii.c_str()), &ascii);
        }
    }
    // A rope of separately built pieces, and a slice across leaf boundaries.
    const ProtoString* rope = ProtoString::fromUTF8(ctx, "");
    std::string expected;
    for (int i = 0; i < 50; ++i) {
        std::string piece = "piece-" + std::to_string(i) + "-" + std::string(i % 40, 'x') + encode(0x3B1 + i);
        rope = rope->appendLast(ctx, ProtoString::fromUTF8(ctx, piece.c_str()));
        expected += piece;
    }
    check(rope, &expected);
    const ProtoString* slice = rope->getSlice(ctx, 17, 700);
    check(slice);
    EXPECT_EQ(slice->toStdString(ctx).size() > 0, true);
}

TEST_F(StringToStdString, NonAsciiAcrossLeafBoundaries) {
    // Every width of sequence at every offset around the 36-byte leaf size.
    for (uint32_t cp : {0xE9u, 0x20ACu, 0x1F600u, 0xD800u, 0xDFFFu, 0xFFFFu, 0x10FFFFu}) {
        for (int prefix = 30; prefix < 40; ++prefix) {
            std::string text(prefix, 'p');
            for (int k = 0; k < 20; ++k) text += encode(cp) + "q";
            SCOPED_TRACE(prefix);
            check(ProtoString::fromUTF8(ctx, text.c_str()), &text);
        }
    }
}

TEST_F(StringToStdString, EmbeddedNul) {
    std::string text("ab\0cd", 5);
    text += std::string(40, 'z');
    text += std::string("\0", 1);
    check(fromBuffer(ctx, text), &text);
}

TEST_F(StringToStdString, RandomWellFormedText) {
    std::mt19937 rng(20261026);
    for (int round = 0; round < 300; ++round) {
        std::string text;
        const int n = static_cast<int>(rng() % 300);
        for (int i = 0; i < n; ++i) {
            uint32_t cp;
            switch (rng() % 5) {
                case 0: cp = 0x20 + rng() % 0x5F; break;
                case 1: cp = 0x80 + rng() % 0x780; break;
                case 2: cp = 0x800 + rng() % 0xF800; break;     // includes surrogates
                case 3: cp = 0x10000 + rng() % 0x100000; break;
                default: cp = 'a' + rng() % 26; break;
            }
            text += encode(cp);
        }
        SCOPED_TRACE(round);
        check(fromBuffer(ctx, text), &text);
    }
}

TEST_F(StringToStdString, MalformedLeavesMatchTheOldOutput) {
    // Bytes no constructor writes: a block copy would return them as they
    // are, the 3.3.0 conversion did not.
    const std::vector<std::vector<uint8_t>> cases = {
        {0x80},                              // lone continuation byte
        {'a', 0xBF, 'b'},
        {0xC0, 0x80},                        // overlong NUL
        {0xE0, 0x80, 0x80, 'x'},             // overlong 3-byte
        {0xF7, 0xBF, 0xBF, 0xBF},            // above U+10FFFF: dropped
        {0xF8, 0x88, 0x80, 0x80, 'y'},
        {0xFF, 'z'},
        {'a', 'b', 0xE2},                    // truncated at the end of the leaf
        {0xC3},
    };
    for (const auto& bytes : cases) {
        const ProtoString* s = rawLeafString(ctx, bytes);
        check(s);
        EXPECT_NE(s->toStdString(ctx), std::string(bytes.begin(), bytes.end()))
            << "this case does not exercise the decode path";
    }
    // A full 36-byte leaf ending with a lead byte: the old decoder read past
    // the payload, into the leaf's hash.
    std::vector<uint8_t> full(35, 'f');
    full.push_back(0xF0);
    check(rawLeafString(ctx, full));
    // Malformed leaves inside a rope, next to well-formed ones.
    check(rawRope(ctx, {{'o', 'k'}, {0xC3}, {0xA9, 'x'}, std::vector<uint8_t>(36, 'm'), {0xF4, 0x90, 0x80, 0x80}}));
}

TEST_F(StringToStdString, RandomBytesInLeaves) {
    std::mt19937 rng(7);
    for (int round = 0; round < 2000; ++round) {
        std::vector<std::vector<uint8_t>> leaves(1 + rng() % 4);
        for (auto& leaf : leaves) {
            leaf.resize(rng() % (StringLeafNode::MAX_PAYLOAD + 1));
            for (auto& b : leaf) b = static_cast<uint8_t>(rng() % 3 == 0 ? rng() : 'a' + rng() % 26);
        }
        SCOPED_TRACE(round);
        check(rawRope(ctx, leaves));
    }
}
