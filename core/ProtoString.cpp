/*
 * ProtoString.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 *
 * This file implements the ProtoString and its iterator.
 * The string is implemented as a rope, which is a tree structure
 * where the leaves are tuples of characters. This allows for efficient
 * concatenation and slicing operations.
 */

#include "../headers/proto_internal.h"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace proto {

    // =========================================================================
    // UTF-8 utilities
    // =========================================================================

    // Precondition: 'lead' is the first byte of a valid UTF-8 sequence (not a continuation byte).
    static uint32_t utf8SeqLen(uint8_t lead) {
        if (lead < 0x80) return 1;
        if (lead < 0xE0) return 2;
        if (lead < 0xF0) return 3;
        return 4;
    }

    static uint32_t decodeCodepoint(const uint8_t* p) {
        const uint8_t lead = p[0];
        if (lead < 0x80) return lead;
        if (lead < 0xE0) return static_cast<uint32_t>((lead & 0x1Fu) << 6u)  | (p[1] & 0x3Fu);
        if (lead < 0xF0) return static_cast<uint32_t>((lead & 0x0Fu) << 12u) | (static_cast<uint32_t>(p[1] & 0x3Fu) << 6u)  | (p[2] & 0x3Fu);
        return               static_cast<uint32_t>((lead & 0x07u) << 18u) | (static_cast<uint32_t>(p[1] & 0x3Fu) << 12u) | (static_cast<uint32_t>(p[2] & 0x3Fu) << 6u) | (p[3] & 0x3Fu);
    }

    static uint64_t fnv1a(const uint8_t* data, size_t len) {
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < len; ++i)
            h = (h ^ data[i]) * 1099511628211ULL;
        return h;
    }

    static uint64_t hashCombine(uint64_t a, uint64_t b) {
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    }

    // =========================================================================
    // Forward declarations for AVL primitives and UTF-8 output helper.
    // These are defined later in the file but are needed by
    // ProtoStringImplementation methods and RopeCharacterIterator.
    // =========================================================================
    const ProtoObject* strConcat(ProtoContext* ctx,
                                  const ProtoObject* a,
                                  const ProtoObject* b);
    struct SplitResult { const ProtoObject* left; const ProtoObject* right; };
    SplitResult strSplit(ProtoContext* ctx, const ProtoObject* node, uint32_t char_index);
    void strCharAt(const ProtoObject* node, uint32_t index, uint32_t* out);
    static void appendUTF8CodePoint(std::string& out, unsigned int codepoint);
    const ProtoStringImplementation* internString(ProtoContext* context, const ProtoStringImplementation* newString);

    // =========================================================================
    // StringLeafNode
    // =========================================================================

    StringLeafNode::StringLeafNode(ProtoContext* ctx,
                                   const uint8_t* bytes, uint8_t byte_cnt,
                                   uint16_t char_cnt, bool partial)
        : Cell(ctx)
        , byte_count(byte_cnt)
        , _pad_char_count(0)
        , char_count(char_cnt)
        , flags(partial ? 1u : 0u)
        , _pad{}
        , content_hash(computeHash(bytes, byte_cnt))
    {
        std::memcpy(utf8_payload, bytes, byte_cnt);
    }

    uint64_t StringLeafNode::computeHash(const uint8_t* bytes, uint8_t len) {
        return fnv1a(bytes, len);
    }

    uint32_t StringLeafNode::charToByteOffset(uint32_t char_index) const {
        uint32_t byte_pos = 0;
        uint32_t char_pos = 0;
        while (char_pos < char_index && byte_pos < static_cast<uint32_t>(byte_count)) {
            byte_pos += utf8SeqLen(utf8_payload[byte_pos]);
            ++char_pos;
        }
        return byte_pos;
    }

    uint32_t StringLeafNode::codepointAt(uint32_t byte_pos) const {
        return decodeCodepoint(utf8_payload + byte_pos);
    }

    const ProtoObject* StringLeafNode::asObject() const {
        ProtoObjectPointer pa{};
        pa.stringLeafNode = this;
        pa.op.pointer_tag = POINTER_TAG_STRING_LEAF_NODE;
        return pa.oid;
    }

    const StringLeafNode* StringLeafNode::fromObject(const ProtoObject* obj) {
        return toImpl<const StringLeafNode>(obj);
    }

    bool StringLeafNode::isStringLeafNode(const ProtoObject* obj) {
        if (!obj) return false;
        ProtoObjectPointer pa{};
        pa.oid = obj;
        return pa.op.pointer_tag == POINTER_TAG_STRING_LEAF_NODE;
    }

    const ProtoObject* StringLeafNode::implAsObject(ProtoContext* /*context*/) const {
        return asObject();
    }

    //=========================================================================
    // Inline string helpers (tagged pointer; up to 7 UTF-32 chars in 54 bits)
    //=========================================================================

    bool isInlineString(const ProtoObject* o) {
        if (!o) return false;
        ProtoObjectPointer pa{};
        pa.oid = o;
        return pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING;
    }

    static unsigned long inlineStringLength(const ProtoObject* o) {
        ProtoObjectPointer pa{};
        pa.oid = o;
        return pa.inlineString.inline_byte_count;
    }

    // Forward declaration — defined later in the AVL helpers section.
    static const ProtoStringImplementation* getImpl(const ProtoObject* obj);

    /** Returns the Unicode codepoint at index i for an inline ASCII string (0..127). */
    static unsigned int inlineStringCharAt(const ProtoObject* o, int i) {
        ProtoObjectPointer pa{};
        pa.oid = o;
        return static_cast<unsigned int>((pa.inlineString.inline_utf8_bytes >> (static_cast<unsigned>(i) * 8u)) & 0xFFu);
    }

    static unsigned long getProtoStringSize(ProtoContext* /*context*/, const ProtoObject* o) {
        if (isInlineString(o)) {
            // Count Unicode codepoints, not raw bytes.
            unsigned long bc = inlineStringByteCount(o);
            unsigned long chars = 0;
            for (unsigned long i = 0; i < bc; ) {
                i += utf8SeqLen(inlineStringByte(o, i));
                ++chars;
            }
            return chars;
        }
        return static_cast<unsigned long>(getImpl(o)->implGetSize());
    }

    static const ProtoObject* getProtoStringGetAt(ProtoContext* context, const ProtoObject* o, int index) {
        if (isInlineString(o)) {
            const unsigned int cp = inlineStringCharAt(o, index);
            return context->fromUnicodeChar(cp);
        }
        return getImpl(o)->implGetAt(context, index);
    }

    const ProtoObject* createInlineString(ProtoContext* context, int len, const unsigned int* codepoints) {
        // Pack ASCII codepoints as UTF-8 bytes (one byte each) into the inline_utf8_bytes field.
        // Layout: pointer_tag(6) | embedded_type(4) | inline_byte_count(3) | inline_utf8_bytes(48) | reserved(3)
        ProtoObjectPointer pa{};
        pa.oid = nullptr;
        pa.inlineString.pointer_tag   = POINTER_TAG_EMBEDDED_VALUE;
        pa.inlineString.embedded_type = EMBEDDED_TYPE_INLINE_STRING;
        const int byte_count = (len < INLINE_STRING_MAX_BYTES) ? len : INLINE_STRING_MAX_BYTES;
        pa.inlineString.inline_byte_count = static_cast<unsigned long>(byte_count);
        unsigned long packed = 0;
        for (int i = 0; i < byte_count; ++i)
            packed |= (static_cast<unsigned long>(codepoints[i] & 0xFF) << (static_cast<unsigned>(i) * 8u));
        pa.inlineString.inline_utf8_bytes = packed;
        pa.inlineString.reserved = 0;
        return reinterpret_cast<const ProtoObject*>(pa.oid);
    }

    const ProtoObject* createInlineStringUTF8(ProtoContext* /*context*/,
                                               const uint8_t* bytes,
                                               uint8_t byte_count) {
        assert(byte_count <= INLINE_STRING_MAX_BYTES);
        assert(!byte_count || bytes != nullptr);
        ProtoObjectPointer pa{};
        pa.oid = nullptr;
        pa.inlineString.pointer_tag       = POINTER_TAG_EMBEDDED_VALUE;
        pa.inlineString.embedded_type     = EMBEDDED_TYPE_INLINE_STRING;
        pa.inlineString.inline_byte_count = byte_count;
        unsigned long packed = 0;
        for (uint8_t i = 0; i < byte_count; ++i)
            packed |= (static_cast<unsigned long>(bytes[i]) << (i * 8u));
        pa.inlineString.inline_utf8_bytes = packed;
        pa.inlineString.reserved = 0;
        return pa.oid;
    }

    // =========================================================================
    // Helpers for ProtoString public operations (AVL-based)
    // =========================================================================

    /** Unwrap any ProtoString* to its ProtoStringImplementation (for POINTER_TAG_STRING or POINTER_TAG_SYMBOL).
     *  Bypasses toImpl's strict single-tag check because both POINTER_TAG_STRING and POINTER_TAG_SYMBOL
     *  use the same underlying ProtoStringImplementation layout; only the tag differs. */
    static const ProtoStringImplementation* getImpl(const ProtoObject* obj) {
        if (!obj) return nullptr;
        ProtoObjectPointer pa{}; pa.oid = obj;
        unsigned long tag = pa.op.pointer_tag;
        if (tag == POINTER_TAG_STRING || tag == POINTER_TAG_SYMBOL) {
            // Manually clear the lower 6 tag bits to obtain the raw Cell pointer.
            uintptr_t raw = reinterpret_cast<uintptr_t>(obj) & ~static_cast<uintptr_t>(0x3F);
            return reinterpret_cast<const ProtoStringImplementation*>(raw);
        }
        return nullptr;
    }

    /** Materialize any string representation to its AVL root node. */
    static const ProtoObject* getRoot(ProtoContext* ctx, const ProtoObject* strObj) {
        if (!strObj) return nullptr;
        if (isInlineString(strObj)) {
            unsigned long bc = inlineStringByteCount(strObj);
            if (bc == 0) return nullptr;
            uint8_t bytes[INLINE_STRING_MAX_BYTES];
            for (unsigned long i = 0; i < bc; ++i)
                bytes[i] = inlineStringByte(strObj, i);
            uint16_t chars = 0;
            for (unsigned long i = 0; i < bc; ) {
                i += utf8SeqLen(bytes[i]);
                ++chars;
            }
            return (new(ctx) StringLeafNode(ctx, bytes, static_cast<uint8_t>(bc), chars))->asObject();
        }
        auto* impl = getImpl(strObj);
        return impl ? impl->avl_root : nullptr;
    }

    /** Wrap an AVL root back to the best ProtoString representation (inline if small enough, interned otherwise). */
    static const ProtoObject* wrapRoot(ProtoContext* ctx, const ProtoObject* root) {
        if (!root) {
            return createInlineStringUTF8(ctx, nullptr, 0);
        }
        // For a leaf node, copy raw UTF-8 bytes directly — no decode/re-encode needed.
        // This correctly handles all codepoint widths (1-4 bytes).
        if (StringLeafNode::isStringLeafNode(root)) {
            auto* leaf = StringLeafNode::fromObject(root);
            if (leaf->byte_count <= INLINE_STRING_MAX_BYTES) {
                return createInlineStringUTF8(ctx, leaf->utf8_payload, leaf->byte_count);
            }
        } else {
            // Internal node: collect codepoints and re-encode only when the byte count fits.
            uint32_t totalBytes = StringInternalNode::byteCount(root);
            if (totalBytes <= INLINE_STRING_MAX_BYTES) {
                uint8_t buf[INLINE_STRING_MAX_BYTES];
                uint32_t pos = 0;
                uint32_t totalChars = StringInternalNode::charCount(root);
                for (uint32_t i = 0; i < totalChars; ++i) {
                    uint32_t cp = 0;
                    strCharAt(root, i, &cp);
                    if (cp < 0x80u) {
                        buf[pos++] = static_cast<uint8_t>(cp);
                    } else if (cp < 0x800u) {
                        buf[pos++] = static_cast<uint8_t>(0xC0u | (cp >> 6));
                        buf[pos++] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
                    } else if (cp < 0x10000u) {
                        buf[pos++] = static_cast<uint8_t>(0xE0u | (cp >> 12));
                        buf[pos++] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
                        buf[pos++] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
                    } else {
                        buf[pos++] = static_cast<uint8_t>(0xF0u | (cp >> 18));
                        buf[pos++] = static_cast<uint8_t>(0x80u | ((cp >> 12) & 0x3Fu));
                        buf[pos++] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
                        buf[pos++] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
                    }
                }
                return createInlineStringUTF8(ctx, buf, static_cast<uint8_t>(pos));
            }
        }
        auto* pendingStr = new(ctx) ProtoStringImplementation(ctx, root);
        ctx->pendingRoot = const_cast<ProtoStringImplementation*>(pendingStr);
        const ProtoStringImplementation* interned = internString(ctx, pendingStr);
        ctx->pendingRoot = nullptr;
        return interned->implAsObject(ctx);
    }

    /** O(N) rope/AVL traversal: iterates codepoints without repeated descent.
     *
     *  For StringLeafNode entries the iterator tracks the *byte* position
     *  within the leaf's utf8_payload and decodes UTF-8 inline, giving
     *  true O(1) amortized cost per codepoint (no charToByteOffset scan).
     *
     *  For legacy Tuple-based rope nodes the behaviour is unchanged.
     */
    class RopeCharacterIterator {
        static constexpr int MAX_DEPTH = 64;
        const ProtoObject* stack[MAX_DEPTH];
        int slotIndex[MAX_DEPTH];     // For leaves: byte offset; for others: state index.
        int top = -1;
        ProtoContext* context;

    public:
        RopeCharacterIterator(ProtoContext* ctx, const ProtoObject* obj) : context(ctx) {
            if (obj) push(obj);
        }

        void push(const ProtoObject* obj) {
            if (top + 1 >= MAX_DEPTH) std::abort();
            stack[++top] = obj;
            slotIndex[top] = 0;
        }

        unsigned int next() {
            while (top >= 0) {
                const ProtoObject* current = stack[top];
                int& idx = slotIndex[top];

                if (isInlineString(current)) {
                    // idx tracks the current byte position within the inline UTF-8 payload.
                    uint8_t lead = inlineStringByte(current, static_cast<unsigned long>(idx));
                    unsigned long bc = inlineStringByteCount(current);
                    if (static_cast<unsigned long>(idx) < bc) {
                        uint8_t raw[4] = {0, 0, 0, 0};
                        uint32_t seqLen = utf8SeqLen(lead);
                        for (uint32_t b = 0; b < seqLen && static_cast<unsigned long>(idx) + b < bc; ++b)
                            raw[b] = inlineStringByte(current, static_cast<unsigned long>(idx) + b);
                        idx += static_cast<int>(seqLen);
                        return static_cast<unsigned int>(decodeCodepoint(raw));
                    }
                    top--; continue;
                }

                if (current->isString(context)) {
                    const ProtoStringImplementation* sImpl = getImpl(current);
                    if (idx == 0) {
                        idx = 1;
                        if (sImpl && sImpl->avl_root) push(sImpl->avl_root);
                        continue;
                    }
                    top--; continue;
                }

                if (StringLeafNode::isStringLeafNode(current)) {
                    const StringLeafNode* leaf = StringLeafNode::fromObject(current);
                    // idx tracks the current *byte* position within utf8_payload.
                    uint32_t bytePos = static_cast<uint32_t>(idx);
                    if (bytePos < static_cast<uint32_t>(leaf->byte_count)) {
                        uint32_t cp = decodeCodepoint(leaf->utf8_payload + bytePos);
                        idx += static_cast<int>(utf8SeqLen(leaf->utf8_payload[bytePos]));
                        return static_cast<unsigned int>(cp);
                    }
                    top--; continue;
                }

                if (StringInternalNode::isStringInternalNode(current)) {
                    const StringInternalNode* node = StringInternalNode::fromObject(current);
                    if (idx == 0) {
                        idx = 1;
                        if (node->right) push(node->right);
                        if (node->left)  push(node->left);
                        continue;
                    }
                    top--; continue;
                }

                if (current->isTuple(context)) {
                    const ProtoTupleImplementation* tImpl = toImpl<const ProtoTupleImplementation>(current);
                    if (idx < TUPLE_SIZE) {
                        const ProtoObject* child = tImpl->slot[idx++];
                        if (child) {
                            if (child->isString(context) || child->isTuple(context)) {
                                push(child);
                            } else {
                                return extractCodePoint(child);
                            }
                        }
                        continue;
                    }
                    top--; continue;
                }

                // Fallback for non-tuple/string objects or unknowns
                if (idx == 0) {
                    idx = 1;
                    return extractCodePoint(current);
                }
                top--;
            }
            return 0; // End of iteration
        }

        bool hasNext(ProtoContext* ctx) const {
            if (top < 0) return false;

            // State-preserving lookahead
            int tempTop = top;
            int tempSlotIndex[MAX_DEPTH];
            const ProtoObject* tempStack[MAX_DEPTH];
            for(int i=0; i<=top; ++i) {
                tempSlotIndex[i] = slotIndex[i];
                tempStack[i] = stack[i];
            }

            while (tempTop >= 0) {
                const ProtoObject* current = tempStack[tempTop];
                int& idx = tempSlotIndex[tempTop];

                if (isInlineString(current)) {
                    // idx is a byte position; check against byte count.
                    if (idx < (int)inlineStringByteCount(current)) return true;
                    tempTop--; continue;
                }

                if (current->isString(ctx)) {
                    if (idx == 0) {
                        idx = 1;
                        const ProtoStringImplementation* sImpl = getImpl(current);
                        const ProtoObject* root = sImpl ? sImpl->avl_root : nullptr;
                        if (root) {
                            tempTop++;
                            if (tempTop >= MAX_DEPTH) return false;
                            tempStack[tempTop] = root;
                            tempSlotIndex[tempTop] = 0;
                        }
                        continue;
                    }
                    tempTop--; continue;
                }

                if (StringLeafNode::isStringLeafNode(current)) {
                    const StringLeafNode* leaf = StringLeafNode::fromObject(current);
                    if (static_cast<uint32_t>(idx) < static_cast<uint32_t>(leaf->byte_count)) return true;
                    tempTop--; continue;
                }

                if (StringInternalNode::isStringInternalNode(current)) {
                    if (idx == 0) {
                        // Not yet expanded — children exist, so more codepoints remain.
                        const StringInternalNode* node = StringInternalNode::fromObject(current);
                        idx = 1;
                        if (node->right) {
                            tempTop++;
                            if (tempTop >= MAX_DEPTH) return false;
                            tempStack[tempTop] = node->right;
                            tempSlotIndex[tempTop] = 0;
                        }
                        if (node->left) {
                            tempTop++;
                            if (tempTop >= MAX_DEPTH) return false;
                            tempStack[tempTop] = node->left;
                            tempSlotIndex[tempTop] = 0;
                        }
                        continue;
                    }
                    tempTop--; continue;
                }

                if (current->isTuple(ctx)) {
                    const ProtoTupleImplementation* tImpl = toImpl<const ProtoTupleImplementation>(current);
                    if (idx < TUPLE_SIZE) {
                        const ProtoObject* child = tImpl->slot[idx++];
                        if (child) {
                            if (child->isString(ctx) || child->isTuple(ctx)) {
                                tempTop++;
                                if (tempTop >= MAX_DEPTH) return false;
                                tempStack[tempTop] = child;
                                tempSlotIndex[tempTop] = 0;
                            } else {
                                return true;
                            }
                        }
                        continue;
                    }
                    tempTop--; continue;
                }

                if (idx == 0) return true;
                tempTop--;
            }
            return false;
        }

    private:
        unsigned int extractCodePoint(const ProtoObject* obj) const {
            ProtoObjectPointer pa{}; pa.oid = obj;
            if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_UNICODE_CHAR) {
                return static_cast<unsigned int>(pa.unicodeChar.unicodeValue & 0x1FFFFFu);
            } else if (obj && obj->isInteger(context)) {
                return static_cast<unsigned int>(obj->asLong(context) & 0x1FFFFFu);
            }
            return 0;
        }
    };

    static int compareStrings(ProtoContext* context, const ProtoObject* s1, const ProtoObject* s2) {
        if (s1 == s2) return 0;

        const unsigned long len1 = getProtoStringSize(context, s1);
        const unsigned long len2 = getProtoStringSize(context, s2);
        const unsigned long minLen = std::min(len1, len2);

        // Use RopeCharacterIterator for both sides to ensure correct UTF-8 decoding
        // regardless of whether strings are inline (potentially multi-byte) or rope-based.
        RopeCharacterIterator it1(context, s1);
        RopeCharacterIterator it2(context, s2);
        for (unsigned long i = 0; i < minLen; ++i) {
            unsigned int cp1 = it1.next();
            unsigned int cp2 = it2.next();
            if (cp1 < cp2) return -1;
            if (cp1 > cp2) return 1;
        }

        if (len1 < len2) return -1;
        if (len1 > len2) return 1;
        return 0;
    }

    unsigned long getProtoStringHash(ProtoContext* context, const ProtoObject* o) {
        if (isInlineString(o)) {
            unsigned long h = 0;
            const unsigned long len = inlineStringLength(o);
            for (unsigned long i = 0; i < len; ++i)
                h = (h * 31UL) + static_cast<unsigned long>(inlineStringCharAt(o, static_cast<int>(i)));
            return h;
        }
        return getImpl(o)->getHash(context);
    }

    //=========================================================================
    // ProtoStringIteratorImplementation
    //
    // O(1) amortised per-codepoint traversal using cached leaf state.
    //
    // Strategy:
    //   - totalSize is cached at construction → implHasNext() is always O(1).
    //   - currentLeaf tracks the active StringLeafNode; leafBytePos tracks the
    //     byte offset within that leaf's utf8_payload.  Within a leaf every
    //     next() call is pure O(1) UTF-8 decode with no tree traversal.
    //   - When a leaf is exhausted (or on the very first next() call after
    //     construction), locateLeaf() descends the AVL tree from the root to
    //     find the leaf that contains charIndex.  This descent is O(log N), but
    //     because a leaf holds up to 32 bytes (≈10–32 codepoints) the amortised
    //     cost per codepoint is O(log N / leafSize) ≈ O(1).
    //   - Inline strings (all ASCII, ≤6 bytes) are handled without any tree
    //     descent: inlineStringCharAt() decodes them in O(1).
    //=========================================================================

    ProtoStringIteratorImplementation::ProtoStringIteratorImplementation(
        ProtoContext* context,
        const ProtoObject* stringObj,
        uint32_t i
    ) : Cell(context)
      , base(stringObj)
      , totalSize(stringObj ? static_cast<uint32_t>(getProtoStringSize(context, stringObj)) : 0u)
      , charIndex(i)
      , currentLeaf(nullptr)
      , leafBytePos(0u)
    {
        std::memset(_pad, 0, sizeof(_pad));
    }

    // O(1): totalSize was cached at construction; no tree traversal needed.
    int ProtoStringIteratorImplementation::implHasNext(ProtoContext* /*context*/) const {
        return this->base && this->charIndex < this->totalSize;
    }

    const ProtoObject* ProtoStringIteratorImplementation::implNext(ProtoContext* context) {
        if (!this->base || this->charIndex >= this->totalSize) return nullptr;

        // Fast path: inline string — decode UTF-8 from the inline payload.
        // leafBytePos is repurposed here as the byte offset within the inline payload.
        if (isInlineString(this->base)) {
            uint8_t raw[4] = {0, 0, 0, 0};
            unsigned long bc = inlineStringByteCount(this->base);
            uint8_t lead = inlineStringByte(this->base, static_cast<unsigned long>(this->leafBytePos));
            uint32_t seqLen = utf8SeqLen(lead);
            for (uint32_t b = 0; b < seqLen && static_cast<unsigned long>(this->leafBytePos) + b < bc; ++b)
                raw[b] = inlineStringByte(this->base, static_cast<unsigned long>(this->leafBytePos) + b);
            this->leafBytePos = static_cast<uint8_t>(this->leafBytePos + seqLen);
            this->charIndex++;
            return context->fromUnicodeChar(decodeCodepoint(raw));
        }

        // AVL string: if currentLeaf is exhausted or not yet set, descend to the
        // leaf that contains charIndex.
        if (this->currentLeaf == nullptr ||
            static_cast<uint32_t>(this->leafBytePos) >= static_cast<uint32_t>(this->currentLeaf->byte_count)) {
            const ProtoStringImplementation* sImpl = getImpl(this->base);
            locateLeaf(sImpl ? sImpl->avl_root : nullptr);
        }

        if (!this->currentLeaf) {
            // Fallback: should not happen for a well-formed string; return null.
            this->charIndex++;
            return nullptr;
        }

        // Decode the next codepoint from the current leaf's UTF-8 payload.
        assert(this->leafBytePos < this->currentLeaf->byte_count);
        const uint32_t seqLen = utf8SeqLen(this->currentLeaf->utf8_payload[this->leafBytePos]);
        const uint32_t cp = decodeCodepoint(this->currentLeaf->utf8_payload + this->leafBytePos);
        this->leafBytePos = static_cast<uint8_t>(this->leafBytePos + seqLen);
        this->charIndex++;
        return context->fromUnicodeChar(cp);
    }

    void ProtoStringIteratorImplementation::locateLeaf(const ProtoObject* node) {
        // Descend the AVL tree to find the leaf that contains codepoint at charIndex.
        // Tracks how many codepoints precede the current subtree so that we can
        // compute the correct intra-leaf byte offset.
        uint32_t remaining = this->charIndex;

        while (node) {
            if (StringInternalNode::isStringInternalNode(node)) {
                const StringInternalNode* n = StringInternalNode::fromObject(node);
                if (remaining < n->left_chars) {
                    node = n->left;
                } else {
                    remaining -= n->left_chars;
                    node = n->right;
                }
            } else if (StringLeafNode::isStringLeafNode(node)) {
                const StringLeafNode* leaf = StringLeafNode::fromObject(node);
                // remaining is the codepoint index within this leaf.
                this->currentLeaf = leaf;
                this->leafBytePos = static_cast<uint8_t>(leaf->charToByteOffset(remaining));
                return;
            } else {
                // Unknown node type — stop traversal.
                break;
            }
        }

        // Could not locate a leaf (e.g., empty AVL tree).
        this->currentLeaf = nullptr;
        this->leafBytePos = 0;
    }

    // Note: each call to implAdvance() is O(log N) — it allocates a new iterator with a cleared
    // leaf cache, forcing a fresh AVL descent on the next implNext() call.  Use the mutable
    // nextCodepoint() path (implNext on a reused iterator) for O(N) full traversal instead.
    const ProtoStringIteratorImplementation* ProtoStringIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (this->base && this->charIndex < this->totalSize) {
            return new (context) ProtoStringIteratorImplementation(context, this->base, this->charIndex + 1);
        }
        return this;
    }

    const ProtoObject* ProtoStringIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.stringIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING_ITERATOR;
        return p.oid;
    }

    void ProtoStringIteratorImplementation::finalize(ProtoContext* /*context*/) const {}

    void ProtoStringIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
            )
    ) const {
        // Mark the root string object.
        if (this->base && !isInlineString(this->base)) {
            const Cell* c = this->base->asCell(context);
            if (c && ProtoObject::isCellPointer(reinterpret_cast<const ProtoObject*>(c))) {
                method(context, self, ProtoObject::asCellPointer(reinterpret_cast<const ProtoObject*>(c)));
            }
        }
        // Mark the cached leaf so the GC does not collect it while the iterator is live.
        if (this->currentLeaf) {
            method(context, self, this->currentLeaf);
        }
    }

    unsigned long ProtoStringIteratorImplementation::getHash(ProtoContext* /*context*/) const {
        return reinterpret_cast<uintptr_t>(this);
    }

    const ProtoStringIterator* ProtoStringIteratorImplementation::asProtoStringIterator(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.stringIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING_ITERATOR;
        return p.stringIterator;
    }


    // ===== Content hash for interning (structure-independent) ===================

    /** Compute FNV-1a over all leaf bytes in left-to-right order.
     *  This produces the same hash regardless of AVL tree structure. */
    uint64_t computeContentHash(const ProtoObject* node) {
        if (!node) return 14695981039346656037ULL;  // FNV offset basis
        if (StringLeafNode::isStringLeafNode(node)) {
            auto* leaf = StringLeafNode::fromObject(node);
            return fnv1a(leaf->utf8_payload, leaf->byte_count);
        }
        if (StringInternalNode::isStringInternalNode(node)) {
            // In-order traversal: collect all leaf bytes sequentially
            // Use a stack-based approach to avoid recursion
            struct Frame { const ProtoObject* n; };
            Frame stack[64];
            int top = 0;
            stack[top++] = {node};
            uint64_t h = 14695981039346656037ULL;
            while (top > 0) {
                const ProtoObject* cur = stack[--top].n;
                if (!cur) continue;
                if (StringLeafNode::isStringLeafNode(cur)) {
                    auto* leaf = StringLeafNode::fromObject(cur);
                    for (uint8_t i = 0; i < leaf->byte_count; ++i)
                        h = (h ^ leaf->utf8_payload[i]) * 1099511628211ULL;
                } else if (StringInternalNode::isStringInternalNode(cur)) {
                    auto* n = StringInternalNode::fromObject(cur);
                    // Push right first so left is processed first (stack is LIFO)
                    stack[top++] = {n->right};
                    stack[top++] = {n->left};
                }
            }
            return h;
        }
        return 14695981039346656037ULL;
    }

    // ===== ProtoStringImplementation (wraps AVL root) ==========================

    ProtoStringImplementation::ProtoStringImplementation(ProtoContext* ctx,
                                                          const ProtoObject* root)
        : Cell(ctx), avl_root(root) {}

    uint32_t ProtoStringImplementation::implGetSize() const {
        return StringInternalNode::charCount(avl_root);
    }

    uint64_t ProtoStringImplementation::implGetHash() const {
        return StringInternalNode::subtreeHash(avl_root);
    }

    const ProtoObject* ProtoStringImplementation::implAsObject(ProtoContext* /*ctx*/) const {
        ProtoObjectPointer pa{};
        pa.stringImplementation = this;
        pa.op.pointer_tag = POINTER_TAG_STRING;
        return pa.oid;
    }

    const ProtoString* ProtoStringImplementation::asProtoString(ProtoContext* /*ctx*/) const {
        ProtoObjectPointer p{};
        p.stringImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING;
        return p.string;
    }

    const ProtoStringImplementation* ProtoStringImplementation::implAsSymbol(ProtoContext* /*ctx*/) const {
        ProtoObjectPointer pa{};
        pa.symbolImplementation = this;
        pa.op.pointer_tag = POINTER_TAG_SYMBOL;
        return reinterpret_cast<const ProtoStringImplementation*>(pa.oid);
    }

    // Build a balanced AVL tree from a contiguous UTF-8 byte array.
    static const ProtoObject* buildAVL(ProtoContext* ctx,
                                        const uint8_t* bytes, size_t len) {
        if (len == 0) return nullptr;

        // Count Unicode codepoints in this segment.
        uint16_t char_cnt = 0;
        for (size_t i = 0; i < len; ) {
            i += utf8SeqLen(bytes[i]);
            ++char_cnt;
        }

        if (len <= StringLeafNode::MAX_PAYLOAD) {
            return (new(ctx) StringLeafNode(ctx,
                bytes, static_cast<uint8_t>(len), char_cnt))->asObject();
        }

        // Split roughly in half at a codepoint boundary.
        size_t mid_byte = len / 2;
        // Back up to a lead byte if we landed in the middle of a multibyte sequence.
        while (mid_byte > 0 && (bytes[mid_byte] & 0xC0u) == 0x80u) --mid_byte;

        const ProtoObject* left  = buildAVL(ctx, bytes, mid_byte);
        const ProtoObject* right = buildAVL(ctx, bytes + mid_byte, len - mid_byte);
        return strConcat(ctx, left, right);
    }

    const ProtoStringImplementation* ProtoStringImplementation::fromUTF8Bytes(
            ProtoContext* ctx, const uint8_t* bytes, size_t len) {
        return new(ctx) ProtoStringImplementation(ctx, buildAVL(ctx, bytes, len));
    }

    void ProtoStringImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext*, void*, const Cell*)
    ) const {
        if (avl_root && ProtoObject::isCellPointer(avl_root)) {
            method(context, self, ProtoObject::asCellPointer(avl_root));
        }
    }

    // Legacy compatibility: O(n) character access via AVL charAt.
    const ProtoObject* ProtoStringImplementation::implGetAt(ProtoContext* context, int index) const {
        if (!avl_root || index < 0) return nullptr;
        uint32_t cp = 0;
        strCharAt(avl_root, static_cast<uint32_t>(index), &cp);
        return context->fromUnicodeChar(cp);
    }

    // Legacy compatibility: size via AVL charCount.
    unsigned long ProtoStringImplementation::implGetSizeCompat(ProtoContext* /*context*/) const {
        return static_cast<unsigned long>(implGetSize());
    }

    // Legacy compatibility: convert to list of unicode char objects.
    const ProtoList* ProtoStringImplementation::implAsList(ProtoContext* context) const {
        const ProtoList* list = context->newList();
        const uint32_t size = implGetSize();
        for (uint32_t i = 0; i < size; ++i) {
            uint32_t cp = 0;
            strCharAt(avl_root, i, &cp);
            list = list->appendLast(context, context->fromUnicodeChar(cp));
        }
        return list;
    }

    // Legacy compatibility: append via AVL concat.
    const ProtoStringImplementation* ProtoStringImplementation::implAppendLast(
            ProtoContext* context, const ProtoString* otherString) const {
        // Build a temporary AVL from the other string's characters.
        std::string utf8;
        otherString->toUTF8String(context, utf8);
        const ProtoObject* otherRoot = buildAVL(
            context,
            reinterpret_cast<const uint8_t*>(utf8.data()),
            utf8.size());
        return new(context) ProtoStringImplementation(context,
            strConcat(context, avl_root, otherRoot));
    }

    void ProtoStringImplementation::finalize(ProtoContext* /*context*/) const {}

    unsigned long ProtoStringImplementation::getHash(ProtoContext* /*context*/) const {
        return static_cast<unsigned long>(implGetHash());
    }

    int ProtoStringImplementation::implCompare(ProtoContext* context, const ProtoString* other) const {
        return compareStrings(context, this->implAsObject(context), other->asObject(context));
    }

    const ProtoStringIteratorImplementation* ProtoStringImplementation::implGetIterator(ProtoContext* context) const {
        return new(context) ProtoStringIteratorImplementation(context, this->implAsObject(context), 0);
    }

    //=========================================================================
    // ProtoString API
    //=========================================================================

    unsigned long ProtoString::getSize(ProtoContext* context) const {
        auto* self = reinterpret_cast<const ProtoObject*>(this);
        if (isInlineString(self)) {
            unsigned long bc = inlineStringByteCount(self);
            unsigned long chars = 0;
            for (unsigned long i = 0; i < bc; ) {
                i += utf8SeqLen(inlineStringByte(self, i));
                ++chars;
            }
            return chars;
        }
        auto* impl = getImpl(self);
        return impl ? static_cast<unsigned long>(impl->implGetSize()) : 0;
    }

    const ProtoObject* ProtoString::getAt(ProtoContext* context, int index) const {
        auto* self = reinterpret_cast<const ProtoObject*>(this);
        const ProtoObject* root = getRoot(context, self);
        if (!root) return nullptr;
        uint32_t cp = 0;
        strCharAt(root, static_cast<uint32_t>(index), &cp);
        return context->fromUnicodeChar(cp);
    }

    const ProtoList* ProtoString::asList(ProtoContext* context) const {
        auto* self = reinterpret_cast<const ProtoObject*>(this);
        const ProtoObject* root = getRoot(context, self);
        const ProtoList* list = context->newList();
        RopeCharacterIterator it(context, root);
        while (it.hasNext(context)) {
            uint32_t cp = it.next();
            list = list->appendLast(context, context->fromUnicodeChar(cp));
        }
        return list;
    }

    const ProtoString* ProtoString::getSlice(ProtoContext* context, int from, int to) const {
        auto* self = reinterpret_cast<const ProtoObject*>(this);
        const unsigned long size = getSize(context);
        if (from < 0) from = 0;
        if (to > static_cast<int>(size)) to = static_cast<int>(size);
        if (from >= to) return ProtoString::fromUTF8String(context, "");
        const ProtoObject* root = getRoot(context, self);
        auto [_, right] = strSplit(context, root, static_cast<uint32_t>(from));
        auto [mid, __]  = strSplit(context, right, static_cast<uint32_t>(to - from));
        return reinterpret_cast<const ProtoString*>(wrapRoot(context, mid));
    }

    int ProtoString::cmp_to_string(ProtoContext* context, const ProtoString* otherString) const {
        auto* a = reinterpret_cast<const ProtoObject*>(this);
        auto* b = reinterpret_cast<const ProtoObject*>(otherString);
        return compareStrings(context, a, b);
    }

    const ProtoObject* ProtoString::asObject(ProtoContext* context) const {
        auto* self = reinterpret_cast<const ProtoObject*>(this);
        if (isInlineString(self)) return self;
        // For both POINTER_TAG_STRING and POINTER_TAG_SYMBOL the tagged pointer
        // already IS the canonical object handle — return it directly.
        ProtoObjectPointer pa{}; pa.oid = self;
        if (pa.op.pointer_tag == POINTER_TAG_STRING || pa.op.pointer_tag == POINTER_TAG_SYMBOL) return self;
        return getImpl(self)->implAsObject(context);
    }

    const ProtoString* ProtoString::setAt(ProtoContext* context, int index, const ProtoObject* character) const {
        // Remove the character at index, then insert the new one
        return removeAt(context, index)->insertAt(context, index, character);
    }

    const ProtoString* ProtoString::insertAt(ProtoContext* context, int index, const ProtoObject* character) const {
        // Encode character to a single-codepoint string, then use insertAtString
        unsigned int cp = 0;
        ProtoObjectPointer pa{}; pa.oid = character;
        if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_UNICODE_CHAR) {
            cp = static_cast<unsigned int>(pa.unicodeChar.unicodeValue & 0x1FFFFFu);
        } else if (character && character->isInteger(context)) {
            cp = static_cast<unsigned int>(character->asLong(context) & 0x1FFFFFu);
        }
        uint8_t buf[4];
        uint8_t len = 0;
        if (cp < 0x80u)         { buf[0] = static_cast<uint8_t>(cp); len = 1; }
        else if (cp < 0x800u)   { buf[0] = 0xC0u | (cp >> 6); buf[1] = 0x80u | (cp & 0x3Fu); len = 2; }
        else if (cp < 0x10000u) { buf[0] = 0xE0u | (cp >> 12); buf[1] = 0x80u | ((cp >> 6) & 0x3Fu); buf[2] = 0x80u | (cp & 0x3Fu); len = 3; }
        else                    { buf[0] = 0xF0u | (cp >> 18); buf[1] = 0x80u | ((cp >> 12) & 0x3Fu); buf[2] = 0x80u | ((cp >> 6) & 0x3Fu); buf[3] = 0x80u | (cp & 0x3Fu); len = 4; }

        auto* charLeaf = new(context) StringLeafNode(context, buf, len, 1);
        auto* self = reinterpret_cast<const ProtoObject*>(this);
        const ProtoObject* root = getRoot(context, self);
        auto [left, right] = strSplit(context, root, static_cast<uint32_t>(index));
        const ProtoObject* result = strConcat(context, strConcat(context, left, charLeaf->asObject()), right);
        return reinterpret_cast<const ProtoString*>(wrapRoot(context, result));
    }

    const ProtoString* ProtoString::setAtString(ProtoContext* context, int index, const ProtoString* otherString) const {
        // Replace characters starting at index with those from otherString
        unsigned long otherLen = otherString->getSize(context);
        const ProtoString* removed = removeSlice(context, index, index + static_cast<int>(otherLen));
        return removed->insertAtString(context, index, otherString);
    }

    const ProtoString* ProtoString::insertAtString(ProtoContext* context, int index, const ProtoString* otherString) const {
        auto* self     = reinterpret_cast<const ProtoObject*>(this);
        auto* charsObj = reinterpret_cast<const ProtoObject*>(otherString);
        const ProtoObject* root = getRoot(context, self);
        auto [left, right] = strSplit(context, root, static_cast<uint32_t>(index));
        const ProtoObject* result = strConcat(context,
                                               strConcat(context, left, getRoot(context, charsObj)),
                                               right);
        return reinterpret_cast<const ProtoString*>(wrapRoot(context, result));
    }

    const ProtoString* ProtoString::splitFirst(ProtoContext* context, int count) const {
        unsigned long size = getSize(context);
        if (count <= 0) return ProtoString::fromUTF8String(context, "");
        if (count >= static_cast<int>(size)) return const_cast<ProtoString*>(this);
        return getSlice(context, 0, count);
    }

    const ProtoString* ProtoString::splitLast(ProtoContext* context, int count) const {
        unsigned long size = getSize(context);
        if (count <= 0) return ProtoString::fromUTF8String(context, "");
        if (count >= static_cast<int>(size)) return const_cast<ProtoString*>(this);
        return getSlice(context, static_cast<int>(size) - count, static_cast<int>(size));
    }

    const ProtoString* ProtoString::removeFirst(ProtoContext* context, int count) const {
        unsigned long size = getSize(context);
        if (count <= 0) return const_cast<ProtoString*>(this);
        if (count >= static_cast<int>(size)) return ProtoString::fromUTF8String(context, "");
        return getSlice(context, count, static_cast<int>(size));
    }

    const ProtoString* ProtoString::removeLast(ProtoContext* context, int count) const {
        unsigned long size = getSize(context);
        if (count <= 0) return const_cast<ProtoString*>(this);
        if (count >= static_cast<int>(size)) return ProtoString::fromUTF8String(context, "");
        return getSlice(context, 0, static_cast<int>(size) - count);
    }

    const ProtoString* ProtoString::removeAt(ProtoContext* context, int index) const {
        return removeSlice(context, index, index + 1);
    }

    const ProtoString* ProtoString::removeSlice(ProtoContext* context, int from, int to) const {
        auto* self = reinterpret_cast<const ProtoObject*>(this);
        const unsigned long size = getSize(context);
        if (from < 0) from = 0;
        if (to > static_cast<int>(size)) to = static_cast<int>(size);
        if (from >= to) return const_cast<ProtoString*>(this);
        const ProtoObject* root = getRoot(context, self);
        auto [left, rest]  = strSplit(context, root, static_cast<uint32_t>(from));
        auto [_,    right] = strSplit(context, rest, static_cast<uint32_t>(to - from));
        return reinterpret_cast<const ProtoString*>(
            wrapRoot(context, strConcat(context, left, right)));
    }

    const ProtoStringIterator* ProtoString::getIterator(ProtoContext* context) const {
        if (isInlineString(reinterpret_cast<const ProtoObject*>(this)))
            return (new (context) ProtoStringIteratorImplementation(context, reinterpret_cast<const ProtoObject*>(this), 0))->asProtoStringIterator(context);
        return getImpl(reinterpret_cast<const ProtoObject*>(this))->implGetIterator(context)->asProtoStringIterator(context);
    }

    static void appendUTF8CodePoint(std::string& out, unsigned int codepoint) {
        if (codepoint < 0x80u) {
            out += static_cast<char>(codepoint);
        } else if (codepoint < 0x800u) {
            out += static_cast<char>(0xC0u | (codepoint >> 6));
            out += static_cast<char>(0x80u | (codepoint & 0x3Fu));
        } else if (codepoint < 0x10000u) {
            out += static_cast<char>(0xE0u | (codepoint >> 12));
            out += static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (codepoint & 0x3Fu));
        } else if (codepoint < 0x110000u) {
            out += static_cast<char>(0xF0u | (codepoint >> 18));
            out += static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu));
            out += static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (codepoint & 0x3Fu));
        }
    }

    void ProtoString::toUTF8String(ProtoContext* context, std::string& out) const {
        auto* self = reinterpret_cast<const ProtoObject*>(this);
        const ProtoObject* root = getRoot(context, self);
        uint32_t sz = StringInternalNode::charCount(root);
        out.clear();
        out.reserve(sz);  // approximate — at least one byte per codepoint
        RopeCharacterIterator it(context, root);
        while (it.hasNext(context)) {
            appendUTF8CodePoint(out, it.next());
        }
    }

    const ProtoString* ProtoString::multiply(ProtoContext* context, const ProtoObject* count) const {
        if (!count->isInteger(context)) return nullptr;
        long long n = count->asLong(context);
        if (n <= 0) return ProtoString::fromUTF8String(context, "");
        if (n == 1) return const_cast<ProtoString*>(this);

        auto* self = reinterpret_cast<const ProtoObject*>(this);
        const ProtoObject* root = getRoot(context, self);
        const ProtoObject* result = root;
        for (long long i = 1; i < n; ++i) {
            result = strConcat(context, result, root);
        }
        return reinterpret_cast<const ProtoString*>(wrapRoot(context, result));
    }

    //=========================================================================
    // String Interning Implementation
    //=========================================================================
} // namespace proto
#include <unordered_set>

namespace proto {
    void initStringInternMap(ProtoSpace* space) {
        space->stringInternMap = new StringInternSet();
    }

    void freeStringInternMap(ProtoSpace* space) {
        delete static_cast<StringInternSet*>(space->stringInternMap);
        space->stringInternMap = nullptr;
    }

    const ProtoStringImplementation* internString(ProtoContext* context, const ProtoStringImplementation* newString) {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        StringInternSet* map = static_cast<StringInternSet*>(context->space->stringInternMap);
        if (!map) return newString; // Should not happen if initialized

        auto it = map->find(newString);
        if (it != map->end()) {
            return *it;
        }

        map->insert(newString);
        return newString;
    }
    const ProtoObject* ProtoString::modulo(ProtoContext* context, const ProtoObject* other) const {
        // TODO: Implement string formatting (%)
        // For now, return PROTO_NONE to avoid the "Objects are not integer types for modulo" crash.
        return PROTO_NONE;
    }

    // ProtoString / ProtoStringIterator external API trampolines (from public API)
    const ProtoString* ProtoString::create(ProtoContext* context, const ProtoList* list) {
        if (!list) return nullptr;
        const unsigned long size = list->getSize(context);

        // Collect codepoints from the list.
        std::string utf8;
        unsigned int codepoints[INLINE_STRING_MAX_BYTES];
        bool allASCII = (size <= INLINE_STRING_MAX_BYTES);

        for (unsigned long i = 0; i < size; ++i) {
            const ProtoObject* charObj = list->getAt(context, static_cast<int>(i));
            unsigned int cp = 0;
            ProtoObjectPointer pa{}; pa.oid = charObj;
            if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_UNICODE_CHAR) {
                cp = static_cast<unsigned int>(pa.unicodeChar.unicodeValue & 0x1FFFFFu);
            } else if (charObj && charObj->isInteger(context)) {
                cp = static_cast<unsigned int>(charObj->asLong(context) & 0x1FFFFFu);
            } else if (charObj && charObj->isString(context)) {
                RopeCharacterIterator it(context, charObj);
                cp = it.next();
            }
            if (i < INLINE_STRING_MAX_BYTES) codepoints[i] = cp;
            if (cp >= 128u) allASCII = false;
            appendUTF8CodePoint(utf8, cp);
        }

        if (allASCII && size <= INLINE_STRING_MAX_BYTES) {
            return reinterpret_cast<const ProtoString*>(createInlineString(context, static_cast<int>(size), codepoints));
        }

        const ProtoStringImplementation* pendingStr = ProtoStringImplementation::fromUTF8Bytes(
            context,
            reinterpret_cast<const uint8_t*>(utf8.data()),
            utf8.size());
        context->pendingRoot = const_cast<ProtoStringImplementation*>(pendingStr);

        const ProtoStringImplementation* interned = internString(context, pendingStr);
        context->pendingRoot = nullptr;

        return interned->asProtoString(context);
    }

    const ProtoString* ProtoString::fromUTF8String(ProtoContext* context, const char* str) {
        const ProtoObject* o = context->fromUTF8String(str);
        if (!o || o == PROTO_NONE) return nullptr;

        ProtoObjectPointer pa{};
        pa.oid = o;
        if (pa.op.pointer_tag == POINTER_TAG_STRING || (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING)) {
            return reinterpret_cast<const ProtoString*>(o);
        }
        return o->asString(context);
    }
    const ProtoString* ProtoString::createSymbol(ProtoContext* ctx, const char* utf8) {
        if (!utf8) utf8 = "";
        size_t len = std::strlen(utf8);

        // Short ASCII paths can live as inline strings — they already guarantee
        // pointer equality without SymbolTable involvement.
        if (len <= INLINE_STRING_MAX_BYTES) {
            // Verify the bytes are pure ASCII (no multi-byte sequences that could
            // exceed codepoint count). If so, create an inline string directly.
            bool allAscii = true;
            for (size_t i = 0; i < len; ++i) {
                if (static_cast<unsigned char>(utf8[i]) >= 0x80u) { allAscii = false; break; }
            }
            if (allAscii) {
                const ProtoObject* inl = createInlineStringUTF8(
                    ctx,
                    reinterpret_cast<const uint8_t*>(utf8),
                    static_cast<uint8_t>(len));
                return reinterpret_cast<const ProtoString*>(inl);
            }
        }

        // General path: build a ProtoStringImplementation, intern it as a symbol.
        const ProtoStringImplementation* impl =
            ProtoStringImplementation::fromUTF8Bytes(
                ctx,
                reinterpret_cast<const uint8_t*>(utf8),
                len);
        const ProtoObject* str_obj = impl->implAsObject(ctx);
        const ProtoObject* sym = ctx->space->symbolTable->intern(ctx, str_obj, /*is_strong=*/true);
        return reinterpret_cast<const ProtoString*>(sym);
    }

    const ProtoString* ProtoString::createSymbol(ProtoContext* ctx, const std::string& s) {
        return ProtoString::createSymbol(ctx, s.c_str());
    }

    unsigned long ProtoString::getHash(ProtoContext* context) const { return getProtoStringHash(context, reinterpret_cast<const ProtoObject*>(this)); }
    const Cell* ProtoString::asCell(ProtoContext* context) const { return isInlineString(reinterpret_cast<const ProtoObject*>(this)) ? nullptr : getImpl(reinterpret_cast<const ProtoObject*>(this)); }
    const ProtoString* ProtoString::appendLast(ProtoContext* context, const ProtoString* other) const {
        auto* self     = reinterpret_cast<const ProtoObject*>(this);
        auto* otherObj = reinterpret_cast<const ProtoObject*>(other);
        const ProtoObject* root = strConcat(context,
                                             getRoot(context, self),
                                             getRoot(context, otherObj));
        return reinterpret_cast<const ProtoString*>(wrapRoot(context, root));
    }

    const ProtoString* ProtoString::appendFirst(ProtoContext* context, const ProtoString* other) const {
        auto* self     = reinterpret_cast<const ProtoObject*>(this);
        auto* otherObj = reinterpret_cast<const ProtoObject*>(other);
        const ProtoObject* root = strConcat(context,
                                             getRoot(context, otherObj),
                                             getRoot(context, self));
        return reinterpret_cast<const ProtoString*>(wrapRoot(context, root));
    }

    int ProtoStringIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoStringIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoStringIterator::next(ProtoContext* context) { return toImpl<ProtoStringIteratorImplementation>(this)->implNext(context); }
    const ProtoStringIterator* ProtoStringIterator::advance(ProtoContext* context) { return toImpl<ProtoStringIteratorImplementation>(this)->implAdvance(context)->asProtoStringIterator(context); }
    const ProtoObject* ProtoStringIterator::asObject(ProtoContext* context) const { return toImpl<const ProtoStringIteratorImplementation>(this)->implAsObject(context); }

    // =========================================================================
    // StringInternalNode
    // =========================================================================

    StringInternalNode::StringInternalNode(ProtoContext* ctx,
                                           const ProtoObject* l,
                                           const ProtoObject* r)
        : Cell(ctx)
        , left(l)
        , right(r)
        , total_chars(charCount(l) + charCount(r))
        , left_chars(charCount(l))
        , total_bytes(byteCount(l) + byteCount(r))
        , _pad_align(0)
        , subtree_hash(hashCombine(subtreeHash(l), subtreeHash(r)))
        , height(static_cast<uint8_t>(1 + std::max(nodeHeight(l), nodeHeight(r))))
        , _pad{}
    {}

    int StringInternalNode::nodeHeight(const ProtoObject* n) {
        if (!n) return 0;
        if (StringLeafNode::isStringLeafNode(n)) return 0;   // leaves have height 0
        if (isStringInternalNode(n))
            return static_cast<int>(fromObject(n)->height);
        return 0;
    }

    int StringInternalNode::balance(const ProtoObject* n) {
        if (!n || !isStringInternalNode(n)) return 0;
        const auto* node = fromObject(n);
        return nodeHeight(node->left) - nodeHeight(node->right);
    }

    uint64_t StringInternalNode::subtreeHash(const ProtoObject* n) {
        if (!n) return 0;
        if (StringLeafNode::isStringLeafNode(n))
            return StringLeafNode::fromObject(n)->content_hash;
        if (isStringInternalNode(n))
            return fromObject(n)->subtree_hash;
        return 0;
    }

    uint32_t StringInternalNode::charCount(const ProtoObject* n) {
        if (!n) return 0;
        if (StringLeafNode::isStringLeafNode(n))
            return StringLeafNode::fromObject(n)->char_count;
        if (isStringInternalNode(n))
            return fromObject(n)->total_chars;
        return 0;
    }

    uint32_t StringInternalNode::byteCount(const ProtoObject* n) {
        if (!n) return 0;
        if (StringLeafNode::isStringLeafNode(n))
            return StringLeafNode::fromObject(n)->byte_count;
        if (isStringInternalNode(n))
            return fromObject(n)->total_bytes;
        return 0;
    }

    const ProtoObject* StringInternalNode::asObject() const {
        ProtoObjectPointer pa{};
        pa.stringInternalNode = this;
        pa.op.pointer_tag = POINTER_TAG_STRING_INTERNAL_NODE;
        return pa.oid;
    }

    const StringInternalNode* StringInternalNode::fromObject(const ProtoObject* obj) {
        return toImpl<const StringInternalNode>(obj);
    }

    bool StringInternalNode::isStringInternalNode(const ProtoObject* obj) {
        if (!obj) return false;
        ProtoObjectPointer pa{}; pa.oid = obj;
        return pa.op.pointer_tag == POINTER_TAG_STRING_INTERNAL_NODE;
    }

    const ProtoObject* StringInternalNode::implAsObject(ProtoContext* /*context*/) const {
        return asObject();
    }

    void StringInternalNode::processReferences(ProtoContext* context, void* self,
                                               void (*method)(ProtoContext*, void*, const Cell*)) const {
        if (left && ProtoObject::isCellPointer(left))
            method(context, self, ProtoObject::asCellPointer(left));
        if (right && ProtoObject::isCellPointer(right))
            method(context, self, ProtoObject::asCellPointer(right));
    }

    // =========================================================================
    // AVL primitives
    // =========================================================================

    static const ProtoObject* makeInternal(ProtoContext* ctx,
                                            const ProtoObject* l,
                                            const ProtoObject* r) {
        return (new(ctx) StringInternalNode(ctx, l, r))->asObject();
    }

    static const ProtoObject* rotateRight(ProtoContext* ctx, const ProtoObject* y) {
        const auto* yn = StringInternalNode::fromObject(y);
        assert(StringInternalNode::isStringInternalNode(yn->left) && "rotateRight: left child must be InternalNode");
        const auto* xn = StringInternalNode::fromObject(yn->left);
        const ProtoObject* new_y = makeInternal(ctx, xn->right, yn->right);
        return makeInternal(ctx, xn->left, new_y);
    }

    static const ProtoObject* rotateLeft(ProtoContext* ctx, const ProtoObject* x) {
        const auto* xn = StringInternalNode::fromObject(x);
        assert(StringInternalNode::isStringInternalNode(xn->right) && "rotateLeft: right child must be InternalNode");
        const auto* yn = StringInternalNode::fromObject(xn->right);
        const ProtoObject* new_x = makeInternal(ctx, xn->left, yn->left);
        return makeInternal(ctx, new_x, yn->right);
    }

    static const ProtoObject* avlRebalance(ProtoContext* ctx, const ProtoObject* node) {
        if (!StringInternalNode::isStringInternalNode(node)) return node;
        const int bal = StringInternalNode::balance(node);
        const auto* n = StringInternalNode::fromObject(node);

        if (bal > 1) {
            if (StringInternalNode::balance(n->left) >= 0)
                return rotateRight(ctx, node);                               // LL case
            const ProtoObject* new_left = rotateLeft(ctx, n->left);
            return rotateRight(ctx, makeInternal(ctx, new_left, n->right));  // LR case
        }
        if (bal < -1) {
            if (StringInternalNode::balance(n->right) <= 0)
                return rotateLeft(ctx, node);                                // RR case
            const ProtoObject* new_right = rotateRight(ctx, n->right);
            return rotateLeft(ctx, makeInternal(ctx, n->left, new_right));   // RL case
        }
        return node;
    }

    const ProtoObject* strConcat(ProtoContext* ctx,
                                  const ProtoObject* a,
                                  const ProtoObject* b) {
        if (!a) return b;
        if (!b) return a;

        const int ha = StringInternalNode::nodeHeight(a);
        const int hb = StringInternalNode::nodeHeight(b);

        if (std::abs(ha - hb) <= 1)
            return makeInternal(ctx, a, b);

        if (ha > hb + 1) {
            const auto* an = StringInternalNode::fromObject(a);
            const ProtoObject* new_right = strConcat(ctx, an->right, b);
            return avlRebalance(ctx, makeInternal(ctx, an->left, new_right));
        } else {
            const auto* bn = StringInternalNode::fromObject(b);
            const ProtoObject* new_left = strConcat(ctx, a, bn->left);
            return avlRebalance(ctx, makeInternal(ctx, new_left, bn->right));
        }
    }

    // ===== AVL split primitive ================================================

    static SplitResult splitLeaf(ProtoContext* ctx,
                                  const StringLeafNode* leaf,
                                  uint32_t char_index) {
        assert(char_index <= static_cast<uint32_t>(leaf->char_count) && "splitLeaf: char_index out of range");
        uint32_t byte_split = leaf->charToByteOffset(char_index);

        const StringLeafNode* l = (byte_split > 0)
            ? new(ctx) StringLeafNode(ctx, leaf->utf8_payload,
                                       static_cast<uint8_t>(byte_split),
                                       static_cast<uint16_t>(char_index), /*partial=*/true)
            : nullptr;

        uint8_t right_bytes = leaf->byte_count - static_cast<uint8_t>(byte_split);
        const StringLeafNode* r = (right_bytes > 0)
            ? new(ctx) StringLeafNode(ctx, leaf->utf8_payload + byte_split, right_bytes,
                                       static_cast<uint16_t>(leaf->char_count - char_index), /*partial=*/true)
            : nullptr;

        return { l ? l->asObject() : nullptr, r ? r->asObject() : nullptr };
    }

    SplitResult strSplit(ProtoContext* ctx,
                         const ProtoObject* node,
                         uint32_t char_index) {
        if (!node)                                              return {nullptr, nullptr};
        uint32_t total = StringInternalNode::charCount(node);
        if (char_index == 0)                                   return {nullptr, node};
        if (char_index >= total)                               return {node, nullptr};

        if (StringLeafNode::isStringLeafNode(node))
            return splitLeaf(ctx, StringLeafNode::fromObject(node), char_index);

        const auto* n = StringInternalNode::fromObject(node);
        if (char_index <= n->left_chars) {
            auto [ll, lr] = strSplit(ctx, n->left, char_index);
            return { ll, strConcat(ctx, lr, n->right) };
        } else {
            auto [rl, rr] = strSplit(ctx, n->right, char_index - n->left_chars);
            return { strConcat(ctx, n->left, rl), rr };
        }
    }

    void strCharAt(const ProtoObject* node, uint32_t index, uint32_t* out) {
        while (StringInternalNode::isStringInternalNode(node)) {
            const auto* n = StringInternalNode::fromObject(node);
            if (index < n->left_chars) {
                node = n->left;
            } else {
                index -= n->left_chars;
                node = n->right;
            }
        }
        if (StringLeafNode::isStringLeafNode(node)) {
            const auto* leaf = StringLeafNode::fromObject(node);
            uint32_t byte_pos = leaf->charToByteOffset(index);
            *out = leaf->codepointAt(byte_pos);
        } else {
            *out = 0;
        }
    }
}
