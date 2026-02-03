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
    // ProtoStringIteratorImplementation
    //=========================================================================

    ProtoStringIteratorImplementation::ProtoStringIteratorImplementation(
        ProtoContext* context,
        const ProtoStringImplementation* s,
        unsigned long i
    ) : Cell(context), base(s), currentIndex(i)
    {
    }

    int ProtoStringIteratorImplementation::implHasNext(ProtoContext* context) const {
        if (!this->base) return false;
        return this->currentIndex < this->base->implGetSize(context);
    }

    const ProtoObject* ProtoStringIteratorImplementation::implNext(ProtoContext* context) {
        return this->base->implGetAt(context, this->currentIndex++);
    }

    const ProtoStringIteratorImplementation* ProtoStringIteratorImplementation::implAdvance(ProtoContext* context) const {
        if (this->currentIndex < this->base->implGetSize(context)) {
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
        if (this->base) {
            method(context, self, this->base);
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
        return this->tuple->implGetAt(context, index);
    }

    unsigned long ProtoStringImplementation::implGetSize(ProtoContext* context) const {
        return this->tuple->implGetSize(context);
    }

    const ProtoList* ProtoStringImplementation::implAsList(ProtoContext* context) const {
        return this->tuple->implAsList(context);
    }

    const ProtoStringImplementation* ProtoStringImplementation::implAppendLast(ProtoContext* context, const ProtoString* otherString) const {
        const auto* otherTuple = toImpl<const ProtoStringImplementation>(otherString)->tuple;
        // This is a simplified append. A real rope implementation would be more complex.
        const ProtoList* l1 = this->tuple->implAsList(context);
        const ProtoList* l2 = otherTuple->implAsList(context);
        // This is not an efficient way to concatenate lists.
        // A proper rope implementation would avoid this linear traversal.
        const ProtoListIterator* it = l2->getIterator(context);
        while(it->hasNext(context)){
            l1 = l1->appendLast(context, it->next(context));
        }
        return new (context) ProtoStringImplementation(context, ProtoTupleImplementation::tupleFromList(context, toImpl<const ProtoListImplementation>(l1)));
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
        return new (context) ProtoStringIteratorImplementation(context, this, 0);
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
        return toImpl<const ProtoStringImplementation>(this)->implGetSize(context);
    }

    const ProtoObject* ProtoString::getAt(ProtoContext* context, int index) const {
        return toImpl<const ProtoStringImplementation>(this)->implGetAt(context, index);
    }

    const ProtoList* ProtoString::asList(ProtoContext* context) const {
        return toImpl<const ProtoStringImplementation>(this)->implAsList(context);
    }

    const ProtoString* ProtoString::getSlice(ProtoContext* context, int start, int end) const {
        const auto* tuple_impl = toImpl<const ProtoStringImplementation>(this)->tuple;
        // This is a simplified slice. A real rope implementation would be more complex.
        const ProtoList* list = tuple_impl->implAsList(context);
        const ProtoList* sublist = context->newList();
        for(int i = start; i < end; ++i) {
            sublist = sublist->appendLast(context, list->getAt(context, i));
        }
        return (new (context) ProtoStringImplementation(context, ProtoTupleImplementation::tupleFromList(context, toImpl<const ProtoListImplementation>(sublist))))->asProtoString(context);
    }

    int ProtoString::cmp_to_string(ProtoContext* context, const ProtoString* otherString) const {
        return toImpl<const ProtoStringImplementation>(this)->implCompare(context, otherString);
    }

    const ProtoObject* ProtoString::asObject(ProtoContext* context) const {
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
        std::lock_guard<std::mutex> lock(ProtoSpace::globalMutex);
        StringInternSet* map = static_cast<StringInternSet*>(context->space->stringInternMap);
        if (!map) return newString; // Should not happen if initialized

        auto it = map->find(newString);
        if (it != map->end()) {
            return *it;
        }
        map->insert(newString);
        return newString;
    }
}
