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

    /** Helper for O(N) rope traversal without repeated descent. */
    class RopeCharacterIterator {
        static constexpr int MAX_DEPTH = 64;
        const ProtoObject* stack[MAX_DEPTH];
        int slotIndex[MAX_DEPTH];
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
                    if (idx < (int)inlineStringLength(current)) {
                        return inlineStringCharAt(current, idx++);
                    }
                    top--; continue; // exhausted this inline string
                }

                if (current->isString(context)) {
                    const ProtoStringImplementation* sImpl = toImpl<const ProtoStringImplementation>(current);
                    if (idx == 0) {
                        idx = 1; // Mark that we've expanded it
                        push(sImpl->tuple->implAsObject(context));
                        continue;
                    }
                    top--; continue; // already expanded
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
                    top--; continue; // exhausted this tuple node
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
            
            // Perform a state-preserving lookahead by simulating exactly one successful `next()` logic pass
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
                    if (idx < (int)inlineStringLength(current)) return true;
                    tempTop--; continue;
                }

                if (current->isString(ctx)) {
                    if (idx == 0) {
                        idx = 1;
                        tempTop++;
                        // Avoid modifying the real stack frame, just simulate pushing tuple
                        if (tempTop >= MAX_DEPTH) return false;
                        tempStack[tempTop] = toImpl<const ProtoStringImplementation>(current)->tuple->implAsObject(ctx);
                        tempSlotIndex[tempTop] = 0;
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

        if (isInlineString(s1) && isInlineString(s2)) {
            for (unsigned long i = 0; i < minLen; ++i) {
                unsigned int cp1 = inlineStringCharAt(s1, static_cast<int>(i));
                unsigned int cp2 = inlineStringCharAt(s2, static_cast<int>(i));
                if (cp1 < cp2) return -1;
                if (cp1 > cp2) return 1;
            }
        } else {
            RopeCharacterIterator it1(context, s1);
            RopeCharacterIterator it2(context, s2);
            for (unsigned long i = 0; i < minLen; ++i) {
                unsigned int cp1 = isInlineString(s1) ? inlineStringCharAt(s1, static_cast<int>(i)) : it1.next();
                unsigned int cp2 = isInlineString(s2) ? inlineStringCharAt(s2, static_cast<int>(i)) : it2.next();
                if (cp1 < cp2) return -1;
                if (cp1 > cp2) return 1;
            }
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
        return compareStrings(context, this->implAsObject(context), other->asObject(context));
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
        const ProtoList* list = context->newList();
        RopeCharacterIterator it(context, reinterpret_cast<const ProtoObject*>(this));
        while (it.hasNext(context)) {
            list = list->appendLast(context, context->fromUnicodeChar(it.next()));
        }
        return list;
    }

    const ProtoString* ProtoString::getSlice(ProtoContext* context, int start, int end) const {
        const unsigned long size = getSize(context);
        if (start < 0) start = 0;
        if (end > static_cast<int>(size)) end = static_cast<int>(size);
        if (start >= end) return ProtoString::fromUTF8String(context, "");
        const ProtoList* sublist = context->newList();
        for (int i = start; i < end; ++i)
            sublist = sublist->appendLast(context, getAt(context, i));
        return ProtoString::create(context, sublist);
    }

    int ProtoString::cmp_to_string(ProtoContext* context, const ProtoString* otherString) const {
        return compareStrings(context, this->asObject(context), otherString->asObject(context));
    }

    const ProtoObject* ProtoString::asObject(ProtoContext* context) const {
        if (isInlineString(reinterpret_cast<const ProtoObject*>(this))) return reinterpret_cast<const ProtoObject*>(this);
        return toImpl<const ProtoStringImplementation>(this)->implAsObject(context);
    }
    
    const ProtoString* ProtoString::setAt(ProtoContext* context, int index, const ProtoObject* character) const {
        const ProtoList* list = asList(context);
        const ProtoList* newList = list->setAt(context, index, character);
        return ProtoString::create(context, newList);
    }
    
    const ProtoString* ProtoString::insertAt(ProtoContext* context, int index, const ProtoObject* character) const {
        const ProtoList* list = asList(context);
        const ProtoList* newList = list->insertAt(context, index, character);
        return ProtoString::create(context, newList);
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
        return ProtoString::create(context, newList);
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
        return ProtoString::create(context, newList);
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
        return ProtoString::create(context, newList);
    }
    
    const ProtoString* ProtoString::removeSlice(ProtoContext* context, int from, int to) const {
        const ProtoList* list = asList(context);
        const ProtoList* newList = list->removeSlice(context, from, to);
        return ProtoString::create(context, newList);
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

        const ProtoList* list = this->asList(context);
        const ProtoList* multipliedList = list->multiply(context, count);
        return ProtoString::create(context, multipliedList);
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
        unsigned long size = list->getSize(context);
        
        if (size <= INLINE_STRING_MAX_LEN) {
            unsigned int codepoints[INLINE_STRING_MAX_LEN];
            bool allASCII = true;
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
                codepoints[i] = cp;
                if (cp >= 128u) allASCII = false;
            }
            if (allASCII) {
                return reinterpret_cast<const ProtoString*>(createInlineString(context, size, codepoints));
            }
        }
        
        const ProtoTuple* tuple = context->newTupleFromList(list);
        return internString(context, new (context) ProtoStringImplementation(context, toImpl<const ProtoTupleImplementation>(tuple)))->asProtoString(context);
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
    unsigned long ProtoString::getHash(ProtoContext* context) const { return getProtoStringHash(context, reinterpret_cast<const ProtoObject*>(this)); }
    const Cell* ProtoString::asCell(ProtoContext* context) const { return isInlineString(reinterpret_cast<const ProtoObject*>(this)) ? nullptr : toImpl<const ProtoStringImplementation>(this); }
    const ProtoString* ProtoString::appendLast(ProtoContext* context, const ProtoString* other) const {
        const ProtoList* leftList = this->asList(context);
        const ProtoList* rightList = other->asList(context);
        return ProtoString::create(context, leftList->extend(context, rightList));
    }

    int ProtoStringIterator::hasNext(ProtoContext* context) const { return toImpl<const ProtoStringIteratorImplementation>(this)->implHasNext(context); }
    const ProtoObject* ProtoStringIterator::next(ProtoContext* context) { return toImpl<ProtoStringIteratorImplementation>(this)->implNext(context); }
    const ProtoStringIterator* ProtoStringIterator::advance(ProtoContext* context) { return toImpl<ProtoStringIteratorImplementation>(this)->implAdvance(context)->asProtoStringIterator(context); }
    const ProtoObject* ProtoStringIterator::asObject(ProtoContext* context) const { return toImpl<const ProtoStringIteratorImplementation>(this)->implAsObject(context); }
}
