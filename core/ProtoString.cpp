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
#include <cstdint>
#include <cstdlib>

namespace proto {

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
        return pa.op.value & 7UL;
    }

    /** Returns 7-bit code unit at index i (0..6). */
    static unsigned int inlineStringCharAt(const ProtoObject* o, int i) {
        ProtoObjectPointer pa{};
        pa.oid = o;
        return static_cast<unsigned int>((pa.op.value >> (3 + 7 * i)) & 0x7F);
    }

    static unsigned long getProtoStringSize(ProtoContext* context, const ProtoObject* o) {
        if (isInlineString(o)) return inlineStringLength(o);
        return toImpl<const ProtoStringImplementation>(o)->implGetSize(context);
    }

    static const ProtoObject* getProtoStringGetAt(ProtoContext* context, const ProtoObject* o, int index) {
        if (isInlineString(o)) {
            const unsigned int cp = inlineStringCharAt(o, index);
            return context->fromUnicodeChar(cp);
        }
        return toImpl<const ProtoStringImplementation>(o)->implGetAt(context, index);
    }

    const ProtoObject* createInlineString(ProtoContext* context, int len, const unsigned int* codepoints) {
        unsigned long value = static_cast<unsigned long>(len & 7);
        for (int i = 0; i < len && i < INLINE_STRING_MAX_LEN; ++i)
            value |= (static_cast<unsigned long>(codepoints[i] & 0x7F) << (3 + 7 * i));
        const uintptr_t ptr = POINTER_TAG_EMBEDDED_VALUE | (static_cast<uintptr_t>(EMBEDDED_TYPE_INLINE_STRING) << 6) | (value << 10);
        return reinterpret_cast<const ProtoObject*>(ptr);
    }

    unsigned long getProtoStringHash(ProtoContext* context, const ProtoObject* o) {
        if (isInlineString(o)) {
            unsigned long h = 0;
            const unsigned long len = inlineStringLength(o);
            for (unsigned long i = 0; i < len; ++i)
                h = (h * 31UL) + static_cast<unsigned long>(inlineStringCharAt(o, static_cast<int>(i)));
            return h;
        }
        return toImpl<const ProtoStringImplementation>(o)->getHash(context);
    }

    //=========================================================================
    // ProtoStringIteratorImplementation (base = string as ProtoObject*, inline or cell)
    //=========================================================================

    ProtoStringIteratorImplementation::ProtoStringIteratorImplementation(
        ProtoContext* context,
        const ProtoObject* stringObj,
        unsigned long i
    ) : Cell(context), base(stringObj), currentIndex(i)
    {
    }

    int ProtoStringIteratorImplementation::implHasNext(ProtoContext* context) const {
        if (!this->base) return false;
        return this->currentIndex < getProtoStringSize(context, this->base);
    }

    const ProtoObject* ProtoStringIteratorImplementation::implNext(ProtoContext* context) {
        return getProtoStringGetAt(context, this->base, static_cast<int>(this->currentIndex++));
    }

    const ProtoStringIteratorImplementation* ProtoStringIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (this->base && this->currentIndex < getProtoStringSize(context, this->base)) {
            return new (context) ProtoStringIteratorImplementation(context, this->base, this->currentIndex + 1);
        }
        return this;
    }

    const ProtoObject* ProtoStringIteratorImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.stringIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING_ITERATOR;
        return p.oid;
    }

    void ProtoStringIteratorImplementation::finalize(ProtoContext* context) const {}

    void ProtoStringIteratorImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
            )
    ) const {
        if (this->base && !isInlineString(this->base)) {
            const Cell* c = this->base->asCell(context);
            if (c) method(context, self, c);
        }
    }

    unsigned long ProtoStringIteratorImplementation::getHash(ProtoContext* context) const {
        return reinterpret_cast<uintptr_t>(this);
    }
    
    const ProtoStringIterator* ProtoStringIteratorImplementation::asProtoStringIterator(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.stringIteratorImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING_ITERATOR;
        return p.stringIterator;
    }


    //=========================================================================
    // ProtoStringImplementation
    //=========================================================================

    ProtoStringImplementation::ProtoStringImplementation(
        ProtoContext* context,
        const ProtoTupleImplementation* baseTuple
    ) : Cell(context), tuple(baseTuple)
    {
    }

    const ProtoObject* ProtoStringImplementation::implGetAt(ProtoContext* context, int index) const {
        // A rope node (concat) always has exactly 2 children in slots 0 and 1, and these children are strings.
        // We check this structure instead of relying on the total character count 'actual_size'.
        if (this->tuple->slot[0] && this->tuple->slot[1] && 
            this->tuple->slot[0]->isString(context) && this->tuple->slot[1]->isString(context) &&
            this->tuple->slot[2] == nullptr && this->tuple->slot[3] == nullptr) {
            const unsigned long leftSize = getProtoStringSize(context, this->tuple->slot[0]);
            if (index >= 0 && static_cast<unsigned long>(index) < leftSize)
                return getProtoStringGetAt(context, this->tuple->slot[0], index);
            const unsigned long rightSize = getProtoStringSize(context, this->tuple->slot[1]);
            if (static_cast<unsigned long>(index) < leftSize + rightSize)
                return getProtoStringGetAt(context, this->tuple->slot[1], index - static_cast<int>(leftSize));
        }
        return this->tuple->implGetAt(context, index);
    }

    unsigned long ProtoStringImplementation::implGetSize(ProtoContext* context) const {
        return this->tuple->implGetSize(context);
    }

    const ProtoList* ProtoStringImplementation::implAsList(ProtoContext* context) const {
        return this->tuple->implAsList(context);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implAppendLast(ProtoContext* context, const ProtoString* otherString) const {
        const ProtoObject* leftObj = this->implAsObject(context);
        const ProtoObject* rightObj = otherString->asObject(context);
        const unsigned long leftSize = this->implGetSize(context);
        const unsigned long rightSize = getProtoStringSize(context, rightObj);
        const ProtoTupleImplementation* concatTuple = ProtoTupleImplementation::tupleConcat(context, leftObj, rightObj, leftSize + rightSize);
        return new (context) ProtoStringImplementation(context, concatTuple);
    }

    void ProtoStringImplementation::finalize(ProtoContext* context) const {}

    void ProtoStringImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(
            ProtoContext* context,
            void* self,
            const Cell* cell
            )
    ) const {
        if (this->tuple) {
            method(context, self, this->tuple);
        }
    }

    const ProtoObject* ProtoStringImplementation::implAsObject(ProtoContext* context) const {
        ProtoObjectPointer p{};
        p.stringImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING;
        return p.oid;
    }

    unsigned long ProtoStringImplementation::getHash(ProtoContext* context) const {
        return this->tuple ? this->tuple->getHash(context) : 0;
    }

    int ProtoStringImplementation::implCompare(ProtoContext* context, const ProtoString* other) const {
        // This is a simplified comparison.
        return this->getHash(context) - other->getHash(context);
    }

    const ProtoStringIteratorImplementation* ProtoStringImplementation::implGetIterator(ProtoContext* context) const {
        return new (context) ProtoStringIteratorImplementation(context, this->implAsObject(context), 0);
    }

    const ProtoString* ProtoStringImplementation::asProtoString(ProtoContext* context) const {
        ProtoObjectPointer p;
        p.stringImplementation = this;
        p.op.pointer_tag = POINTER_TAG_STRING;
        return p.string;
    }

    //=========================================================================
    // ProtoString API
    //=========================================================================

    unsigned long ProtoString::getSize(ProtoContext* context) const {
        const ProtoObject* self = reinterpret_cast<const ProtoObject*>(this);
        if (isInlineString(self)) return inlineStringLength(self);
        return toImpl<const ProtoStringImplementation>(this)->implGetSize(context);
    }

    const ProtoObject* ProtoString::getAt(ProtoContext* context, int index) const {
        const ProtoObject* self = reinterpret_cast<const ProtoObject*>(this);
        if (isInlineString(self)) return context->fromUnicodeChar(inlineStringCharAt(self, index));
        return toImpl<const ProtoStringImplementation>(this)->implGetAt(context, index);
    }

    const ProtoList* ProtoString::asList(ProtoContext* context) const {
        const ProtoObject* self = reinterpret_cast<const ProtoObject*>(this);
        if (isInlineString(self)) {
            const unsigned long len = inlineStringLength(self);
            const ProtoList* list = context->newList();
            for (unsigned long i = 0; i < len; ++i)
                list = list->appendLast(context, context->fromUnicodeChar(inlineStringCharAt(self, static_cast<int>(i))));
            return list;
        }
        return toImpl<const ProtoStringImplementation>(this)->implAsList(context);
    }

    const ProtoString* ProtoString::getSlice(ProtoContext* context, int start, int end) const {
        const unsigned long size = getSize(context);
        if (start < 0) start = 0;
        if (end > static_cast<int>(size)) end = static_cast<int>(size);
        if (start >= end) return ProtoString::fromUTF8String(context, "");
        const ProtoList* sublist = context->newList();
        for (int i = start; i < end; ++i)
            sublist = sublist->appendLast(context, getAt(context, i));
        return (new (context) ProtoStringImplementation(context, ProtoTupleImplementation::tupleFromList(context, toImpl<const ProtoListImplementation>(sublist))))->asProtoString(context);
    }

    int ProtoString::cmp_to_string(ProtoContext* context, const ProtoString* otherString) const {
        const ProtoObject* self = reinterpret_cast<const ProtoObject*>(this);
        if (isInlineString(self)) {
            const unsigned long h = getProtoStringHash(context, self);
            const unsigned long oh = getProtoStringHash(context, otherString->asObject(context));
            return (h < oh) ? -1 : (h > oh) ? 1 : 0;
        }
        return toImpl<const ProtoStringImplementation>(this)->implCompare(context, otherString);
    }

    const ProtoObject* ProtoString::asObject(ProtoContext* context) const {
        if (isInlineString(reinterpret_cast<const ProtoObject*>(this))) return reinterpret_cast<const ProtoObject*>(this);
        return toImpl<const ProtoStringImplementation>(this)->implAsObject(context);
    }
    
    const ProtoString* ProtoString::setAt(ProtoContext* context, int index, const ProtoObject* character) const {
        const ProtoList* list = asList(context);
        const ProtoList* newList = list->setAt(context, index, character);
        const ProtoTuple* tuple = context->newTupleFromList(newList);
        return (new (context) ProtoStringImplementation(context, toImpl<const ProtoTupleImplementation>(tuple)))->asProtoString(context);
    }
    
    const ProtoString* ProtoString::insertAt(ProtoContext* context, int index, const ProtoObject* character) const {
        const ProtoList* list = asList(context);
        const ProtoList* newList = list->insertAt(context, index, character);
        const ProtoTuple* tuple = context->newTupleFromList(newList);
        return (new (context) ProtoStringImplementation(context, toImpl<const ProtoTupleImplementation>(tuple)))->asProtoString(context);
    }
    
    const ProtoString* ProtoString::setAtString(ProtoContext* context, int index, const ProtoString* otherString) const {
        // Convert to list, replace range, convert back
        const ProtoList* list = asList(context);
        const ProtoList* otherList = otherString->asList(context);
        const ProtoListIterator* it = otherList->getIterator(context);
        const ProtoList* newList = list;
        int i = index;
        while (it->hasNext(context) && i < (int)list->getSize(context)) {
            newList = newList->setAt(context, i++, it->next(context));
        }
        const ProtoTuple* tuple = context->newTupleFromList(newList);
        return (new (context) ProtoStringImplementation(context, toImpl<const ProtoTupleImplementation>(tuple)))->asProtoString(context);
    }
    
    const ProtoString* ProtoString::insertAtString(ProtoContext* context, int index, const ProtoString* otherString) const {
        const ProtoList* list = asList(context);
        const ProtoList* otherList = otherString->asList(context);
        const ProtoListIterator* it = otherList->getIterator(context);
        const ProtoList* newList = list;
        int i = index;
        while (it->hasNext(context)) {
            newList = newList->insertAt(context, i++, it->next(context));
        }
        const ProtoTuple* tuple = context->newTupleFromList(newList);
        return (new (context) ProtoStringImplementation(context, toImpl<const ProtoTupleImplementation>(tuple)))->asProtoString(context);
    }
    
    const ProtoString* ProtoString::splitFirst(ProtoContext* context, int count) const {
        unsigned long size = getSize(context);
        if (count <= 0) return ProtoString::fromUTF8String(context, "");
        if (count >= (int)size) return const_cast<ProtoString*>(this);
        return getSlice(context, 0, count);
    }
    
    const ProtoString* ProtoString::splitLast(ProtoContext* context, int count) const {
        unsigned long size = getSize(context);
        if (count <= 0) return ProtoString::fromUTF8String(context, "");
        if (count >= (int)size) return const_cast<ProtoString*>(this);
        return getSlice(context, size - count, size);
    }
    
    const ProtoString* ProtoString::removeFirst(ProtoContext* context, int count) const {
        unsigned long size = getSize(context);
        if (count <= 0) return const_cast<ProtoString*>(this);
        if (count >= (int)size) return ProtoString::fromUTF8String(context, "");
        return getSlice(context, count, size);
    }
    
    const ProtoString* ProtoString::removeLast(ProtoContext* context, int count) const {
        unsigned long size = getSize(context);
        if (count <= 0) return const_cast<ProtoString*>(this);
        if (count >= (int)size) return ProtoString::fromUTF8String(context, "");
        return getSlice(context, 0, size - count);
    }
    
    const ProtoString* ProtoString::removeAt(ProtoContext* context, int index) const {
        const ProtoList* list = asList(context);
        const ProtoList* newList = list->removeAt(context, index);
        // Convert list back to string - simplified approach
        const ProtoTuple* tuple = context->newTupleFromList(newList);
        return (new (context) ProtoStringImplementation(context, toImpl<const ProtoTupleImplementation>(tuple)))->asProtoString(context);
    }
    
    const ProtoString* ProtoString::removeSlice(ProtoContext* context, int from, int to) const {
        const ProtoList* list = asList(context);
        const ProtoList* newList = list->removeSlice(context, from, to);
        const ProtoTuple* tuple = context->newTupleFromList(newList);
        return (new (context) ProtoStringImplementation(context, toImpl<const ProtoTupleImplementation>(tuple)))->asProtoString(context);
    }
    
    const ProtoStringIterator* ProtoString::getIterator(ProtoContext* context) const {
        if (isInlineString(reinterpret_cast<const ProtoObject*>(this)))
            return (new (context) ProtoStringIteratorImplementation(context, reinterpret_cast<const ProtoObject*>(this), 0))->asProtoStringIterator(context);
        return toImpl<const ProtoStringImplementation>(this)->implGetIterator(context)->asProtoStringIterator(context);
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
        out.clear();
        const unsigned long size = getSize(context);
        for (unsigned long i = 0; i < size; ++i) {
            const ProtoObject* ch = getAt(context, static_cast<int>(i));
            if (!ch) continue;
            unsigned int codepoint = 0;
            ProtoObjectPointer pa{};
            pa.oid = ch;
            if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE && pa.op.embedded_type == EMBEDDED_TYPE_UNICODE_CHAR) {
                codepoint = static_cast<unsigned int>(pa.unicodeChar.unicodeValue & 0x1FFFFFu);
            } else if (ch->isInteger(context)) {
                codepoint = static_cast<unsigned int>(ch->asLong(context) & 0x1FFFFFu);
            }
            appendUTF8CodePoint(out, codepoint);
        }
    }

    const ProtoString* ProtoString::multiply(ProtoContext* context, const ProtoObject* count) const {
        if (!count->isInteger(context)) return nullptr;
        long long n = count->asLong(context);
        if (n <= 0) return ProtoString::fromUTF8String(context, "");
        if (n == 1) return const_cast<ProtoString*>(this);

        const ProtoObject* self = reinterpret_cast<const ProtoObject*>(this);
        const ProtoObject* acc = nullptr;
        const ProtoObject* current = self;
        unsigned long current_size = getSize(context);
        unsigned long acc_size = 0;

        unsigned long ucount = static_cast<unsigned long>(n);
        while (ucount > 0) {
            if (ucount & 1) {
                if (!acc) {
                    acc = current;
                    acc_size = current_size;
                } else {
                    const ProtoTupleImplementation* concatTuple = ProtoTupleImplementation::tupleConcat(context, acc, current, acc_size + current_size);
                    acc = (new (context) ProtoStringImplementation(context, concatTuple))->implAsObject(context);
                    acc_size += current_size;
                }
            }
            ucount >>= 1;
            if (ucount > 0) {
                const ProtoTupleImplementation* concatTuple = ProtoTupleImplementation::tupleConcat(context, current, current, current_size + current_size);
                current = (new (context) ProtoStringImplementation(context, concatTuple))->implAsObject(context);
                current_size += current_size;
            }
        }
        return reinterpret_cast<const ProtoString*>(acc);
    }

    //=========================================================================
    // String Interning Implementation
    //=========================================================================
} // namespace proto
#include <unordered_set>

namespace proto {
    struct StringInternHash {
        std::size_t operator()(const ProtoStringImplementation* s) const {
            // Context is not easily available here for getHash...
            // But getHash implementation for String just delegates to tuple->getHash(context).
            // We need context to compute hash OR assume hash is cached/stable?
            // Tuple hash is computed from content.
            // Problem: unordered_set calls hasher without context.
            // But s->tuple pointer is stable and unique for unique content (because Tuple is interned).
            // So we can just hash the tuple POINTER!
            return std::hash<const void*>{}(s->tuple);
        }
    };

    struct StringInternEqual {
        bool operator()(const ProtoStringImplementation* a, const ProtoStringImplementation* b) const {
            // Since tuples are interned, identical strings MUST share the same tuple pointer.
            return a->tuple == b->tuple;
        }
    };

    using StringInternSet = std::unordered_set<const ProtoStringImplementation*, StringInternHash, StringInternEqual>;

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
}
