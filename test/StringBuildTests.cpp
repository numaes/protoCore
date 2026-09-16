// StringBuildTests.cpp — invariants of protoCore string construction.
//
// These tests pin the observable result of building a string, so that changes
// to *how* it is built cannot change *what* is built:
//
//   * every leaf's char_count matches a recount of its own payload bytes
//     (the invariant a hoisted codepoint count could break);
//   * every internal node's total_chars / left_chars / total_bytes agree with
//     its children;
//   * getSize matches an independent codepoint count of the produced bytes;
//   * content, content hash, comparison order, representation (inline vs heap
//     rope) and rope shape (leaf count, internal count, depth) match the
//     values the pre-change implementation produced, captured as goldens.
//
// The golden table was captured from protoCore at ff532024 before any change
// to the string builder, with the probe in
// .agent_scratch/protoCore-strings/probes/corpus.cpp.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unistd.h>

using namespace proto;

namespace {

// ---- local UTF-8 helpers, independent of the implementation ---------------

size_t seqLenOf(uint8_t lead) {
    if (lead < 0x80u) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    return 4;
}

// Counts codepoints the way the rope does: one per lead byte, a trailing
// truncated sequence counting as one.
size_t countCodepoints(const uint8_t* b, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; ) { i += seqLenOf(b[i]); ++n; }
    return n;
}

size_t countCodepoints(const std::string& s) {
    return countCodepoints(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string hexOf(const std::string& s) {
    static const char* d = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) { out += d[c >> 4]; out += d[c & 0xF]; }
    return out;
}

std::string asciiN(size_t n) {
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) s += char('a' + (i % 26));
    return s;
}

std::string repeatUnit(const char* unit, size_t times) {
    std::string s;
    const size_t u = std::char_traits<char>::length(unit);
    s.reserve(u * times);
    for (size_t i = 0; i < times; ++i) s.append(unit, u);
    return s;
}

// ---- representation / rope inspection -------------------------------------

const ProtoStringImplementation* implOf(const ProtoObject* o) {
    if (!o) return nullptr;
    ProtoObjectPointer pa{}; pa.oid = o;
    const unsigned long tag = pa.op.pointer_tag;
    if (tag == POINTER_TAG_STRING || tag == POINTER_TAG_SYMBOL) {
        const uintptr_t raw =
            reinterpret_cast<uintptr_t>(o) & ~static_cast<uintptr_t>(0x3F);
        return reinterpret_cast<const ProtoStringImplementation*>(raw);
    }
    return nullptr;
}

bool isInlineRep(const ProtoObject* o) {
    ProtoObjectPointer pa{}; pa.oid = o;
    return pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE &&
           pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING;
}

struct RopeShape {
    long leaves = 0;
    long internals = 0;
    long leafBytes = 0;
    long leafChars = 0;
    int  depth = 0;
};

// Walks the rope, accumulating the shape and asserting that every node's
// cached counts agree with the bytes underneath it.
void walkRope(const ProtoObject* n, RopeShape& sh, int d, const char* label) {
    if (!n) return;
    if (d > sh.depth) sh.depth = d;

    if (StringLeafNode::isStringLeafNode(n)) {
        const auto* leaf = StringLeafNode::fromObject(n);
        sh.leaves++;
        sh.leafBytes += leaf->byte_count;
        sh.leafChars += leaf->char_count;
        // The invariant a hoisted/!hoisted codepoint count must preserve:
        // a leaf's char_count is the codepoint count of its own payload.
        EXPECT_EQ(static_cast<size_t>(leaf->char_count),
                  countCodepoints(leaf->utf8_payload, leaf->byte_count))
            << label << ": leaf char_count disagrees with its payload";
        EXPECT_LE(leaf->byte_count, StringLeafNode::MAX_PAYLOAD) << label;
        return;
    }

    ASSERT_TRUE(StringInternalNode::isStringInternalNode(n))
        << label << ": rope node is neither leaf nor internal";
    const auto* in = StringInternalNode::fromObject(n);
    sh.internals++;
    EXPECT_EQ(in->total_chars,
              StringInternalNode::charCount(in->left) +
              StringInternalNode::charCount(in->right)) << label;
    EXPECT_EQ(in->left_chars, StringInternalNode::charCount(in->left)) << label;
    EXPECT_EQ(in->total_bytes,
              StringInternalNode::byteCount(in->left) +
              StringInternalNode::byteCount(in->right)) << label;
    walkRope(in->left, sh, d + 1, label);
    walkRope(in->right, sh, d + 1, label);
}

RopeShape shapeOf(const ProtoString* s, const char* label) {
    RopeShape sh;
    const auto* impl = implOf(reinterpret_cast<const ProtoObject*>(s));
    if (impl) walkRope(impl->avl_root, sh, 1, label);
    return sh;
}

// ---- the corpus -----------------------------------------------------------

struct Golden {
    const char* name;
    std::string src;
    unsigned long size;      // codepoints
    unsigned long hash;      // content hash
    long leaves;
    long internals;
    int  depth;
    bool inlineForm;
    const char* outHex;      // expected produced bytes; nullptr when large
};

// Goldens captured from ff532024 before the string-builder changes.
std::vector<Golden> corpus() {
    return {
        {"empty",               "",                      0, 14695981039346656037UL,  0,  0, 0, true,  ""},
        {"ascii1",              asciiN(1),               1, 12638187200555641996UL,  0,  0, 0, true,  "61"},
        {"ascii2",              asciiN(2),               2,   620445648566982762UL,  0,  0, 0, true,  "6162"},
        {"ascii3",              asciiN(3),               3, 16654208175385433931UL,  0,  0, 0, true,  "616263"},
        {"ascii4",              asciiN(4),               4, 18165163011005162717UL,  0,  0, 0, true,  "61626364"},
        {"ascii5",              asciiN(5),               5,  7154184807124264104UL,  0,  0, 0, true,  "6162636465"},
        {"ascii6",              asciiN(6),               6, 15567776504244095498UL,  0,  0, 0, true,  "616263646566"},
        {"ascii7",              asciiN(7),               7,  4642726675185563447UL,  1,  0, 1, false, "61626364656667"},
        {"ascii8",              asciiN(8),               8,  2727646559950394989UL,  1,  0, 1, false, "6162636465666768"},
        {"utf8_2byte_1",        "\xC3\xA9",              1,   775207407765167617UL,  1,  0, 1, false, "C3A9"},
        {"utf8_2byte_3",        "\xC3\xA9\xC3\xA9\xC3\xA9",
                                                         3,  6248735914807110505UL,  1,  0, 1, false, "C3A9C3A9C3A9"},
        {"utf8_3byte_1",        "\xE4\xB8\xAD",          1,  2186921048766284500UL,  1,  0, 1, false, "E4B8AD"},
        {"utf8_3byte_2",        "\xE4\xB8\xAD\xE4\xB8\xAD",
                                                         2,  4653907925545105519UL,  1,  0, 1, false, "E4B8ADE4B8AD"},
        {"utf8_4byte_1",        "\xF0\x9F\x98\x80",      1, 18374412943757542024UL,  1,  0, 1, false, "F09F9880"},
        {"combining",           "e\xCC\x81 a\xCC\x80 o\xCC\x82",
                                                         8,  6054595488278041435UL,  1,  0, 1, false, "65CC812061CC80206FCC82"},
        {"ascii32_exact",       asciiN(32),             32, 17329703966457997997UL,  1,  0, 1, false, nullptr},
        {"ascii33",             asciiN(33),             33,  1208933092337694014UL,  2,  1, 2, false, nullptr},
        {"ascii64",             asciiN(64),             64,  9275208735646692041UL,  2,  1, 2, false, nullptr},
        {"ascii467",            asciiN(467),           467,  7674823378295929201UL, 16, 15, 5, false, nullptr},
        {"ascii4096",           asciiN(4096),         4096,  4646423504542175237UL,128,127, 8, false, nullptr},
        {"ascii65536",          asciiN(65536),       65536,  1498516194919556229UL,2048,2047,12,false, nullptr},
        {"utf8_2byte_467",      repeatUnit("\xC3\xA9", 467),
                                                       467, 17983208637607857065UL, 32, 31, 6, false, nullptr},
        {"utf8_3byte_4096",     repeatUnit("\xE4\xB8\xAD", 4096),
                                                      4096, 11454319136226620197UL,512,511,10, false, nullptr},

        // ---- malformed UTF-8: tolerated exactly as before ------------------
        // The builder decodes to codepoints and re-encodes, so malformed input
        // is normalised rather than passed through.  These rows pin that
        // normalisation byte for byte.
        {"m_lone_lead_2",       "\xC3",                  1,   775161228276782755UL,  1,  0, 1, false, "C383"},
        {"m_lead2_then_ascii",  "\xC3" "A",              2,  6871549771144897030UL,  1,  0, 1, false, "C38341"},
        {"m_trunc_3byte",       "\xE4\xB8",              2, 14652567794796899560UL,  1,  0, 1, false, "C3A4C2B8"},
        {"m_trunc_3byte_ascii", "\xE4\xB8" "Z",          3, 13391774417816351350UL,  1,  0, 1, false, "C3A4C2B85A"},
        {"m_trunc_4byte",       "\xF0\x9F\x98",          3,  4821221736834053931UL,  1,  0, 1, false, "C3B0C29FC298"},
        {"m_stray_cont_1",      "\x80",                  1,   776118902904765311UL,  1,  0, 1, false, "C280"},
        // Four continuation bytes decode as ONE codepoint 0 and stay inline.
        {"m_stray_cont_4",      "\x80\x80\x80\x80",      1, 12638153115695167455UL,  0,  0, 0, true,  "00"},
        {"m_overlong_nul",      "\xC0\x80",              1, 12638153115695167455UL,  0,  0, 0, true,  "00"},
        {"m_overlong_c1",       "\xC1\xBF",              1, 12638211389811462638UL,  0,  0, 0, true,  "7F"},
        {"m_five_byte_lead",    "\xF8\x88\x80\x80\x80",  2,  9728368832312511669UL,  1,  0, 1, false, "E88080C280"},
        {"m_ff",                "\xFF",                  1,   775192014602372663UL,  1,  0, 1, false, "C3BF"},
        {"m_surrogate",         "\xED\xA0\x80",          1,  6624692915571543480UL,  1,  0, 1, false, "EDA080"},
        {"m_mixed",             "abc\xC3\xA9\xFF def",   9, 11501672892280938950UL,  1,  0, 1, false, "616263C3A9C3BF20646566"},
        // A run of continuation bytes longer than one leaf: 40 bytes decode to
        // 10 codepoints of 0, i.e. ten NUL bytes.
        {"m_cont_run_40",       repeatUnit("\x80", 40), 10,  7625447167376158605UL,  1,  0, 1, false, "00000000000000000000"},
        {"m_cont_run_200",      repeatUnit("\x80", 200),50,  9712894080799832493UL,  2,  1, 2, false, nullptr},
        {"m_lead_run_40",       repeatUnit("\xC3", 40), 40, 16437702613407666997UL,  4,  3, 3, false, nullptr},
        {"m_mixed_long",        repeatUnit("ab\xC3\xA9\xFF", 30),
                                                       120,  8021544138131661125UL,  8,  7, 4, false, nullptr},
        {"m_ascii_then_trunc",  asciiN(40) + "\xE4\xB8", 42,  2001875717336544560UL, 2,  1, 2, false, nullptr},
    };
}

// ---- the pre-change construction, kept here as the reference --------------
//
// This is what ProtoContext::fromUTF8String did before the byte-oriented
// builder replaced it: decode the bytes to code points, append each one to a
// ProtoList, then hand that list to ProtoString::create (which re-encodes it
// into a std::string and calls the same bottom-up builder). Everything the
// current implementation produces must still equal what this produces,
// including for malformed input.
const ProtoObject* referenceOldBuild(ProtoContext* c, const char* z) {
    unsigned int codepoints[6];
    int count = 0;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(z);
    bool allASCII = true;

    // The inline probe, unchanged.
    while (*s && count <= 6) {
        unsigned int cp;
        int len;
        if (*s < 0x80)              { cp = *s;        len = 1; }
        else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; len = 2; }
        else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; len = 3; }
        else                          { cp = *s & 0x07; len = 4; }
        for (int i = 1; i < len; ++i) {
            if (s[i] == '\0' || (s[i] & 0xC0) != 0x80) { cp = *s; len = 1; break; }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        if (count < 6) codepoints[count] = cp;
        if (cp >= 128u) allASCII = false;
        ++count;
        s += len;
    }
    if (count > 0 && count <= 6 && allASCII && !*s)
        return createInlineString(c, count, codepoints);

    // The old heavy path: an N-element list of code point objects.
    const ProtoList* list = c->newList();
    s = reinterpret_cast<const unsigned char*>(z);
    while (*s) {
        unsigned int cp;
        int len;
        if (*s < 0x80)              { cp = *s;        len = 1; }
        else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; len = 2; }
        else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; len = 3; }
        else                          { cp = *s & 0x07; len = 4; }
        for (int i = 1; i < len; ++i) {
            if (s[i] == '\0' || (s[i] & 0xC0) != 0x80) { cp = *s; len = 1; break; }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        list = list->appendLast(c, c->fromUnicodeChar(cp));
        s += len;
    }
    return ProtoString::create(c, list)->asObject(c);
}

// Sets PROTOCORE_HEAP_LIMIT_CELLS for the lifetime of the object and restores
// whatever was there before. The variable must be set before the ProtoSpace is
// constructed: it is read at the end of construction, and it also caps each
// refill batch, which is what makes the ceiling bind.
class ScopedHeapLimit {
public:
    explicit ScopedHeapLimit(const char* value) {
        if (const char* old = std::getenv(kName)) {
            had_ = true;
            old_ = old;
        }
        ::setenv(kName, value, 1);
    }
    ~ScopedHeapLimit() {
        if (had_) ::setenv(kName, old_.c_str(), 1);
        else      ::unsetenv(kName);
    }
    ScopedHeapLimit(const ScopedHeapLimit&) = delete;
    ScopedHeapLimit& operator=(const ScopedHeapLimit&) = delete;
private:
    static constexpr const char* kName = "PROTOCORE_HEAP_LIMIT_CELLS";
    bool had_ = false;
    std::string old_;
};

// Resident set size in bytes, or 0 when /proc is unavailable.
size_t residentBytes() {
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long total = 0;
    unsigned long resident = 0;
    const int n = std::fscanf(f, "%lu %lu", &total, &resident);
    std::fclose(f);
    if (n != 2) return 0;
    return static_cast<size_t>(resident) *
           static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}

class StringBuildTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* ctx = nullptr;

    void SetUp() override {
        space = new ProtoSpace();
        ctx = new ProtoContext(space);
    }
    void TearDown() override {
        delete ctx;
        delete space;
    }
};

} // namespace

// Every corpus entry must produce exactly the bytes, size, hash,
// representation and rope shape it produced before the builder changed.
TEST_F(StringBuildTest, CorpusMatchesGoldens) {
    for (const Golden& g : corpus()) {
        const ProtoString* s = ProtoString::fromUTF8(ctx, g.src.c_str());
        ASSERT_NE(s, nullptr) << g.name;
        const ProtoObject* o = reinterpret_cast<const ProtoObject*>(s);

        std::string out;
        s->toUTF8String(ctx, out);

        EXPECT_EQ(s->getSize(ctx), g.size) << g.name;
        EXPECT_EQ(s->getHash(ctx), g.hash) << g.name;
        EXPECT_EQ(isInlineRep(o), g.inlineForm) << g.name;
        if (g.outHex) EXPECT_EQ(hexOf(out), g.outHex) << g.name;

        // getSize must equal an independent codepoint count of the bytes
        // actually produced.
        EXPECT_EQ(s->getSize(ctx), countCodepoints(out)) << g.name;

        const RopeShape sh = shapeOf(s, g.name);
        EXPECT_EQ(sh.leaves, g.leaves) << g.name;
        EXPECT_EQ(sh.internals, g.internals) << g.name;
        EXPECT_EQ(sh.depth, g.depth) << g.name;
        if (!g.inlineForm) {
            EXPECT_EQ(static_cast<size_t>(sh.leafBytes), out.size()) << g.name;
            EXPECT_EQ(static_cast<unsigned long>(sh.leafChars), g.size) << g.name;
            // A balanced binary tree over L leaves has exactly L-1 internals.
            EXPECT_EQ(sh.internals, sh.leaves - 1) << g.name;
        }
    }
}

// 1 MiB through the bulk buffer entry point: the same rope the public
// constructor produces, at a size where the recursion is 16 levels deep.
// (The public constructor at 1 MiB is exercised once it no longer builds an
// intermediate list of codepoint objects — see AllocationIsLinearInLength.)
TEST_F(StringBuildTest, OneMebibyteBulkBuildShapeAndContent) {
    const std::string src = asciiN(1048576);
    uint8_t rem[4];
    uint8_t remCount = 0;
    const ProtoString* s = ProtoString::fromUTF8Buffer(
        ctx, reinterpret_cast<const uint8_t*>(src.data()), src.size(),
        nullptr, 0, rem, &remCount);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(remCount, 0u);
    EXPECT_EQ(s->getSize(ctx), 1048576UL);
    EXPECT_EQ(s->getHash(ctx), 7815921241789043789UL);

    const RopeShape sh = shapeOf(s, "ascii1Mi");
    EXPECT_EQ(sh.leaves, 32768);
    EXPECT_EQ(sh.internals, 32767);
    EXPECT_EQ(sh.depth, 16);
    EXPECT_EQ(sh.leafBytes, 1048576);
    EXPECT_EQ(sh.leafChars, 1048576);

    std::string out;
    s->toUTF8String(ctx, out);
    EXPECT_EQ(out, src);
}

// Codepoint counting must be exact for every leaf-boundary alignment, which
// is where an incorrectly hoisted count would show up first.
TEST_F(StringBuildTest, MultiByteSequencesAcrossEveryLeafBoundary) {
    // 2-, 3- and 4-byte sequences at every length around the 32-byte leaf.
    const char* units[] = {"\xC3\xA9", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80"};
    for (const char* unit : units) {
        for (size_t count = 1; count <= 40; ++count) {
            const std::string src = repeatUnit(unit, count);
            const ProtoString* s = ProtoString::fromUTF8(ctx, src.c_str());
            ASSERT_NE(s, nullptr) << unit << " x" << count;
            EXPECT_EQ(s->getSize(ctx), count) << unit << " x" << count;

            std::string out;
            s->toUTF8String(ctx, out);
            EXPECT_EQ(out, src) << unit << " x" << count;

            const RopeShape sh = shapeOf(s, unit);
            if (!isInlineRep(reinterpret_cast<const ProtoObject*>(s))) {
                EXPECT_EQ(static_cast<size_t>(sh.leafBytes), src.size());
                EXPECT_EQ(static_cast<size_t>(sh.leafChars), count);
            }
        }
    }
}

// Content hash, equality and comparison order are content-derived, so they
// must not depend on how the rope was assembled.
TEST_F(StringBuildTest, ComparisonOrderAndEqualityAreContentDerived) {
    const std::string a = asciiN(467);
    const std::string b = asciiN(467) + "z";
    const std::string c = asciiN(467);

    const ProtoString* sa = ProtoString::fromUTF8(ctx, a.c_str());
    const ProtoString* sb = ProtoString::fromUTF8(ctx, b.c_str());
    const ProtoString* sc = ProtoString::fromUTF8(ctx, c.c_str());

    EXPECT_EQ(sa->getHash(ctx), sc->getHash(ctx));
    EXPECT_EQ(sa->cmp_to_string(ctx, sc), 0);
    EXPECT_LT(sa->cmp_to_string(ctx, sb), 0);
    EXPECT_GT(sb->cmp_to_string(ctx, sa), 0);

    // A rope assembled by concatenation compares equal to the same content
    // built in one pass.
    const ProtoString* joined =
        ProtoString::fromUTF8(ctx, asciiN(200).c_str())
            ->appendLast(ctx, ProtoString::fromUTF8(ctx, asciiN(467).substr(200).c_str()));
    EXPECT_EQ(joined->cmp_to_string(ctx, sa), 0);
    EXPECT_EQ(joined->getHash(ctx), sa->getHash(ctx));
}

// The inline representation is part of the contract: up to 6 UTF-8 bytes of
// pure ASCII live in the tagged pointer and allocate nothing.
TEST_F(StringBuildTest, InlineBoundaryPreserved) {
    for (size_t n = 0; n <= 6; ++n) {
        const std::string src = asciiN(n);
        const ProtoString* s = ProtoString::fromUTF8(ctx, src.c_str());
        EXPECT_TRUE(isInlineRep(reinterpret_cast<const ProtoObject*>(s)))
            << "ascii length " << n << " must stay inline";
        EXPECT_EQ(s->asCell(ctx), nullptr) << "inline strings allocate no cell";
    }
    const ProtoString* seven = ProtoString::fromUTF8(ctx, asciiN(7).c_str());
    EXPECT_FALSE(isInlineRep(reinterpret_cast<const ProtoObject*>(seven)));

    // Six UTF-8 *bytes* that are not ASCII are NOT inline today (the probe
    // requires every codepoint below 128), and must stay that way.
    const ProtoString* threeAccents = ProtoString::fromUTF8(ctx, "\xC3\xA9\xC3\xA9\xC3\xA9");
    EXPECT_FALSE(isInlineRep(reinterpret_cast<const ProtoObject*>(threeAccents)));
}

// Symbols: interning, pointer identity and the symbol flag are unaffected by
// how a non-interned string is built.
TEST_F(StringBuildTest, SymbolBehaviourUnchanged) {
    const char* spelling = "a-long-attribute-name";
    const ProtoString* s1 = ProtoString::createSymbol(ctx, spelling);
    const ProtoString* s2 = ProtoString::createSymbol(ctx, spelling);
    EXPECT_EQ(s1, s2) << "repeat createSymbol must return the canonical pointer";
    EXPECT_TRUE(s1->isSymbol());

    const ProtoString* plain = ProtoString::fromUTF8(ctx, spelling);
    EXPECT_FALSE(plain->isSymbol()) << "fromUTF8 must not intern";
    EXPECT_EQ(plain->cmp_to_string(ctx, s1), 0);
    EXPECT_EQ(plain->getHash(ctx), s1->getHash(ctx));

    // Short ASCII symbols are inline and never reach the symbol table.
    const ProtoString* shortSym = ProtoString::createSymbol(ctx, "abc");
    EXPECT_TRUE(isInlineRep(reinterpret_cast<const ProtoObject*>(shortSym)));
    EXPECT_EQ(shortSym, ProtoString::createSymbol(ctx, "abc"));
}

// fromStdString must agree with fromUTF8 for the whole corpus, including the
// malformed rows.
TEST_F(StringBuildTest, FromStdStringAgreesWithFromUTF8) {
    for (const Golden& g : corpus()) {
        const ProtoString* a = ProtoString::fromUTF8(ctx, g.src.c_str());
        const ProtoString* b = ProtoString::fromStdString(ctx, g.src);
        ASSERT_NE(a, nullptr) << g.name;
        ASSERT_NE(b, nullptr) << g.name;
        EXPECT_EQ(a->getSize(ctx), b->getSize(ctx)) << g.name;
        EXPECT_EQ(a->getHash(ctx), b->getHash(ctx)) << g.name;
        EXPECT_EQ(a->cmp_to_string(ctx, b), 0) << g.name;
        std::string oa, ob;
        a->toUTF8String(ctx, oa);
        b->toUTF8String(ctx, ob);
        EXPECT_EQ(oa, ob) << g.name;
    }
}

// The current builder must reproduce, exactly, what the code point list route
// produced — same bytes, same size, same hash, same representation, same rope.
// This covers the malformed corpus in particular: the bytes are decoded and
// re-encoded, so malformed input is normalised, and that normalisation must
// not drift.
TEST_F(StringBuildTest, MatchesTheReferenceCodepointListConstruction) {
    for (const Golden& g : corpus()) {
        const ProtoObject* refObj = referenceOldBuild(ctx, g.src.c_str());
        const ProtoString* ref = reinterpret_cast<const ProtoString*>(refObj);
        const ProtoString* now = ProtoString::fromUTF8(ctx, g.src.c_str());
        ASSERT_NE(ref, nullptr) << g.name;
        ASSERT_NE(now, nullptr) << g.name;

        std::string refBytes, nowBytes;
        ref->toUTF8String(ctx, refBytes);
        now->toUTF8String(ctx, nowBytes);

        EXPECT_EQ(hexOf(nowBytes), hexOf(refBytes)) << g.name;
        EXPECT_EQ(now->getSize(ctx), ref->getSize(ctx)) << g.name;
        EXPECT_EQ(now->getHash(ctx), ref->getHash(ctx)) << g.name;
        EXPECT_EQ(now->cmp_to_string(ctx, ref), 0) << g.name;
        EXPECT_EQ(isInlineRep(reinterpret_cast<const ProtoObject*>(now)),
                  isInlineRep(refObj)) << g.name;
        EXPECT_EQ(now->isSymbol(), ref->isSymbol()) << g.name;

        const RopeShape a = shapeOf(now, g.name);
        const RopeShape b = shapeOf(ref, g.name);
        EXPECT_EQ(a.leaves, b.leaves) << g.name;
        EXPECT_EQ(a.internals, b.internals) << g.name;
        EXPECT_EQ(a.depth, b.depth) << g.name;
        EXPECT_EQ(a.leafBytes, b.leafBytes) << g.name;
        EXPECT_EQ(a.leafChars, b.leafChars) << g.name;
    }
}

// Building a string must cost the cells of the rope it produces — O(N/32) —
// and not the O(N log N) that one-code-point-at-a-time list construction cost.
// This bound fails by two orders of magnitude against the list route.
TEST_F(StringBuildTest, AllocationIsLinearInLength) {
    for (unsigned long n : {4096UL, 65536UL, 1048576UL}) {
        const std::string src = asciiN(n);
        ProtoContext sub(space, ctx, nullptr, nullptr, nullptr, nullptr);
        const unsigned long before = sub.allocatedCellsCount;
        const ProtoString* s = ProtoString::fromUTF8(&sub, src.c_str());
        const unsigned long used = sub.allocatedCellsCount - before;

        ASSERT_NE(s, nullptr) << n;
        ASSERT_EQ(s->getSize(&sub), n) << n;

        // One 32-byte leaf plus one internal node per 32 bytes, plus the
        // wrapper: leaves + (leaves-1) + 1 == 2*ceil(B/32).
        const unsigned long minimum = 2UL * ((n + 31UL) / 32UL);
        EXPECT_LE(used, minimum + minimum / 2UL + 8UL)
            << n << " characters allocated " << used
            << " cells to produce a " << minimum << "-cell rope";
    }
}

// Repeated 467-character builds under PROTOCORE_HEAP_LIMIT_CELLS must complete
// without aborting, let the collector run, and keep both the heap and the
// resident set bounded.
//
// Two details decide the shape of this test. The limit must be set before the
// ProtoSpace is constructed — it is read at the end of construction and caps
// each refill batch, and a limit applied afterwards does not bind because the
// startup pool is already in the freelist. And a fresh space starts with a pool
// of 262,144 cells while one build of a 467-character string now costs 32, so
// the workload has to be big enough to consume that pool before the collector
// is asked for anything at all.
//
// That it takes twenty thousand builds to reach the collector is precisely the
// effect being tested: through the code point list the same build cost 5,108
// cells, which exhausted the pool in about fifty builds, and the GC critical
// section held across the per-character loop meant the thread never submitted
// its young generation, so nothing was ever reclaimable.
TEST(StringBuildHeapLimitTest, RepeatedBuildsUnderAHeapLimitCollectAndStayBounded) {
#ifndef PROTOCORE_GC_REINCLUDE_SURVIVORS
    GTEST_SKIP() << "requires PROTOCORE_GC_REINCLUDE_SURVIVORS: with the "
                    "survivor re-chain compiled out, ProtoContext::safepoint() "
                    "never submits the young generation, so nothing this loop "
                    "allocates can become a collection candidate and the "
                    "workload exhausts the ceiling by design";
#else
    ScopedHeapLimit limit("100000");

    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    // Submit often, the way an embedder does between units of work.
    space.maxAllocatedCellsPerContext = 500;

    const int startupHeap = space.heapSize;
    const std::string src = asciiN(467);

    const uint64_t cyclesBefore = space.getGCCycleCount();
    const size_t rssBefore = residentBytes();

    for (int i = 0; i < 20000; ++i) {
        const ProtoString* s = ProtoString::fromUTF8(ctx, src.c_str());
        ASSERT_NE(s, nullptr) << "build " << i;
        ASSERT_EQ(s->getSize(ctx), 467UL) << "build " << i;
        ctx->safepoint();
    }
    ctx->safepoint();

    EXPECT_GT(space.getGCCycleCount(), cyclesBefore)
        << "no GC cycle ran: the builds never became collectable";
    EXPECT_LE(space.heapSize, startupHeap)
        << "the heap had to grow: " << startupHeap << " -> " << space.heapSize;

    const size_t rssAfter = residentBytes();
    if (rssBefore && rssAfter) {
        const size_t growth = rssAfter > rssBefore ? rssAfter - rssBefore : 0;
        EXPECT_LT(growth, size_t(64) * 1024 * 1024)
            << "resident set grew by " << (growth / (1024 * 1024)) << " MB";
    }
#endif
}

// 1 MiB through the public constructor: same rope as the bulk entry point,
// at a size where the build recurses 16 levels deep.
TEST_F(StringBuildTest, OneMebibytePublicPathShapeAndContent) {
    const std::string src = asciiN(1048576);
    const ProtoString* s = ProtoString::fromUTF8(ctx, src.c_str());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->getSize(ctx), 1048576UL);
    EXPECT_EQ(s->getHash(ctx), 7815921241789043789UL);

    const RopeShape sh = shapeOf(s, "ascii1Mi-public");
    EXPECT_EQ(sh.leaves, 32768);
    EXPECT_EQ(sh.internals, 32767);
    EXPECT_EQ(sh.depth, 16);
    EXPECT_EQ(sh.leafBytes, 1048576);
    EXPECT_EQ(sh.leafChars, 1048576);

    std::string out;
    s->toUTF8String(ctx, out);
    EXPECT_EQ(out, src);
}
