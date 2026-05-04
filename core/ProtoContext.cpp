/*
 * ProtoContext.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <stdexcept>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <cstring>

namespace proto
{
    unsigned long generate_mutable_ref(ProtoContext* context) {
        return context->space->nextMutableRef++;
    }

    /**
     * @class ProtoContext
     * @brief Represents the execution state of a thread, including the call stack
     *        and memory allocation arena.
     */

    /**
     * @brief Constructs a new execution context for a function call.
     * This is the core of the function execution model. It allocates local variable
     * storage and performs argument-to-parameter binding, throwing exceptions on errors.
     */
    ProtoContext::ProtoContext(
        ProtoSpace* space,
        ProtoContext* previous,
        const ProtoList* parameterNames,
        const ProtoList* localNames,
        const ProtoList* args,
        const ProtoSparseList* kwargs,
        size_t totalSlots,
        const ProtoObject** externalSlots
    ) : previous(previous),
        space(space),
        thread(nullptr),
        closureLocals(nullptr),
        automaticLocals(nullptr),
        automaticLocalsCount(0),
        ownsSlots_(false),
        lastAllocatedCell(nullptr),
        allocatedCellsCount(0),
        returnValue(PROTO_NONE),
        currentFileName(nullptr),
        currentLineNumber(0),
        freeCells(nullptr),
        pendingRoot(nullptr)
    {
        // Step 1: Inherit essential services from the parent context.
        // This must be done first so that this context can allocate objects.
        if (previous) {
            this->space = previous->space;
            this->thread = previous->thread;
        } else {
            // This is the root context, it doesn't have a parent thread yet.
            this->thread = nullptr;
        }
        // Auto-detect main thread: if a runtime bootstraps a new ProtoContext
        // without a parent (previous == nullptr), check whether the calling OS
        // thread is the main thread recorded in ProtoSpace.  If so, inherit the
        // main thread's ProtoThreadImplementation so that per-thread caches work
        // immediately — no runtime-specific workaround needed.
        if (!this->thread && this->space
                && this->space->rootContext
                && this->space->rootContext->thread
                && this->space->mainThreadId == std::this_thread::get_id()) {
            this->thread = this->space->rootContext->thread;
        }
        // Step 2: Acquire storage for local variables BEFORE registration.
        // Fast path (externalSlots): caller pre-allocated a stack buffer — zero heap cost.
        // Slow path: heap-allocate and zero-initialise.
        {
            size_t nameCount = localNames ? localNames->getSize(this) : 0;
            automaticLocalsCount = (totalSlots > nameCount) ? totalSlots : nameCount;
            if (externalSlots && automaticLocalsCount <= totalSlots) {
                // Caller owns the buffer and has already initialised it to PROTO_NONE.
                this->automaticLocals = externalSlots;
                ownsSlots_ = false;
            } else if (automaticLocalsCount > 0) {
                const ProtoObject** temp = new const ProtoObject*[automaticLocalsCount];
                std::fill(temp, temp + automaticLocalsCount, PROTO_NONE);
                this->automaticLocals = temp;
                ownsSlots_ = true;
            }
        }
        
        // Step 3: Register context as reachable by GC.
        // closureLocals is intentionally left nullptr here; it is allocated lazily on first
        // parameter binding below. The GC null-checks closureLocals before scanning it, so
        // a nullptr value is safe. Callers that pass parameterNames=nullptr (e.g. protoPython's
        // slot-based calling convention) avoid the allocation entirely.
        if (this->thread) {
            toImpl<ProtoThreadImplementation>(this->thread)->implSetCurrentContext(this);
        } else if (this->space) {
            this->space->mainContext = this;
        }

        // Step 3: Argument to Parameter Binding
        if (!parameterNames) return; // Nothing more to do if there are no parameters.

        // Allocate closureLocals only when we actually need to bind parameters.
        // This is safe: we are registered with the GC above, so any subsequent
        // allocation that triggers GC will find this context on the reachable set.
        closureLocals = this->newSparseList();

        const unsigned int paramCount = parameterNames->getSize(this);
        const unsigned int argCount = args ? args->getSize(this) : 0;

        // 3a. Too many positional arguments
        if (argCount > paramCount) {
            throw std::invalid_argument("Too many positional arguments provided.");
        }

        std::vector<bool> assigned(paramCount, false);

        // GC critical section: every setAt below returns a freshly built
        // SparseList tree.  The result is held in the surrounding
        // expression and only published to this->closureLocals on the
        // assignment that follows, so a concurrent STW root scan
        // landing between the setAt allocations and the assignment
        // would observe the new tree as candidate-but-unreachable.
        // The guard covers both binding loops.
        ProtoContext::CriticalSection cs(this);

        // 3b. Bind positional arguments
        for (unsigned int i = 0; i < argCount; ++i) {
            const ProtoString* paramName = parameterNames->getAt(this, i)->asString(this);
            const ProtoObject* value = args->getAt(this, i);
            closureLocals = closureLocals->setAt(this, paramName->getHash(this), value);
            assigned[i] = true;
        }

        // 3c. Bind keyword arguments
        if (kwargs) {
            const ProtoSparseListIterator* iterator = kwargs->getIterator(this);
            while (iterator->hasNext(this)) {
                unsigned long key = iterator->nextKey(this);
                const ProtoObject* value = iterator->nextValue(this);

                bool found = false;
                for (unsigned int i = 0; i < paramCount; ++i) {
                    const ProtoString* paramName = parameterNames->getAt(this, i)->asString(this);
                    if (paramName->getHash(this) == key) {
                        if (assigned[i]) {
							if (this->space->parameterTwiceAssignedCallback)
								this->space->parameterTwiceAssignedCallback(this, nullptr, paramName);
							continue;
                        }
                        closureLocals = closureLocals->setAt(this, key, value);
                        assigned[i] = true;
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    // To get the name for the error message, we would need a reverse mapping
                    // from hash to string, which is complex. We'll give a generic error.
					if (this->space->parameterNotFoundCallback)
						this->space->parameterNotFoundCallback(this, nullptr, nullptr);
                }
                iterator = const_cast<ProtoSparseListIterator*>(iterator)->advance(this);
            }
        }
    }

    /**
     * @brief Destructor for the context.
     * Reports allocated cells to the GC and frees automatic local variable storage.
     */
    ProtoContext::~ProtoContext()
    {
        if (this->thread) {
            toImpl<ProtoThreadImplementation>(this->thread)->implSetCurrentContext(this->previous);
        } else if (this->space) {
            this->space->mainContext = this->previous;
        }

        if (this->space && this->lastAllocatedCell) {
            this->space->submitYoungGeneration(this->lastAllocatedCell);
        }

        if (this->returnValue && this->previous)
        {
            Cell* cell = const_cast<Cell*>(this->returnValue->asCell(this));
            if (cell != nullptr) {
                (void) new(this->previous) ReturnReference(this->previous, cell);
                // Cell constructor already registers with context via addCell2Context
            }
        }
        // Any unfreed cells from the local batch must be returned to the space
        if (this->freeCells && this->space) {
            while (lock.test_and_set(std::memory_order_acquire)) {}
            std::lock_guard<std::recursive_mutex> freeLock(ProtoSpace::globalMutex);
            Cell* batchTail = this->freeCells;
            int count = 1;
            while (batchTail->getNext()) {
                batchTail = batchTail->getNext();
                count++;
            }
            batchTail->internalSetNextRaw(this->space->freeCells);
            this->space->freeCells = this->freeCells;
            this->space->freeCellsCount += count;
            this->freeCells = nullptr;
            lock.clear(std::memory_order_release);
        }

        // Free the slot array only when this context owns it (heap-allocated path).
        if (ownsSlots_) delete[] automaticLocals;
    }

    void ProtoContext::resizeAutomaticLocals(unsigned int newCount) {
        if (newCount <= automaticLocalsCount) return;
        const ProtoObject** fresh = new const ProtoObject*[newCount];
        std::fill(fresh, fresh + newCount, PROTO_NONE);
        if (automaticLocals && automaticLocalsCount > 0) {
            for (unsigned int i = 0; i < automaticLocalsCount; ++i)
                fresh[i] = automaticLocals[i];
        }
        if (ownsSlots_ && automaticLocals) delete[] automaticLocals;
        automaticLocals = fresh;
        automaticLocalsCount = newCount;
        ownsSlots_ = true;
    }

    /**
     * @brief Cooperative GC safepoint.
     *
     * Fast path: relaxed atomic load of `stwFlag`.  When the flag is clear
     * (the common case) the call costs essentially nothing.
     *
     * Slow path: when the GC has signalled stop-the-world, increment
     * `parkedThreads`, notify the GC, and wait on `stopTheWorldCV` until
     * the flag clears.  This is the same handshake `allocCell` performs
     * every 64 allocations, exposed as a public hook so that an embedder
     * (e.g. protoPython's bytecode dispatcher) can cooperate with the GC
     * from a tight, allocation-free hot loop.
     */
    void ProtoContext::safepoint()
    {
        if (!this || !this->space) return;

#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
        // Per-context allocation-threshold submission.  This is the only
        // place we hand lastAllocatedCell over to dirtySegments — calling
        // it from allocCell mid-construction would orphan in-flight cells
        // held by native helpers in C++ locals.  An embedder calling
        // safepoint() does so at a point where every reachable Cell is
        // anchored from a real GC root (operand stack on automaticLocals,
        // mutableRoot, prototypes, embedder root sets), so a submission
        // here cannot leave anything live but unreachable.
        //
        // Skipped while in a critical section: a wrapper called from
        // inside setAttribute would otherwise be trapped between the
        // construction of a new tree and its CAS into mutableRoot.
        if (this->criticalSectionDepth == 0 &&
            this->space->maxAllocatedCellsPerContext > 0 &&
            this->allocatedCellsCount > this->space->maxAllocatedCellsPerContext &&
            this->space->gcThread &&
            std::this_thread::get_id() != this->space->gcThread->get_id()) {
            while (lock.test_and_set(std::memory_order_acquire)) {}
            Cell* chain = this->lastAllocatedCell;
            this->lastAllocatedCell = nullptr;
            this->allocatedCellsCount = 0;
            lock.clear(std::memory_order_release);
            if (chain) {
                this->space->submitYoungGeneration(chain);
            }
        }
#endif

        if (!this->space->stwFlag.load(std::memory_order_relaxed)) return;
        // GC thread itself never parks against its own STW.
        if (this->space->gcThread &&
            std::this_thread::get_id() == this->space->gcThread->get_id()) return;
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
        // Same critical-section discipline: a thread mid-construction
        // must NOT park; STW would otherwise start with a half-built
        // tree's cells in dirtySegments and unreachable from any root.
        if (this->criticalSectionDepth > 0) return;
#endif
        this->space->parkedThreads++;
        {
            GC_LOCK_TRACE("safepoint STW ACQ");
            std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            this->space->gcCV.notify_all();
            this->space->stopTheWorldCV.wait(lock, [this] { return !this->space->stwFlag.load(); });
            GC_LOCK_TRACE("safepoint STW REL");
        }
        this->space->parkedThreads--;
    }

    /**
     * @brief The core memory allocation function.
     */
    Cell* ProtoContext::allocCell()
    {
        // Poll the stop-the-world flag every 64 allocations instead of every call.
        // A cooperative GC can tolerate a few-microsecond delay; this eliminates 63 out of 64
        // seq_cst atomic loads on the hot allocation path.
        //
        // While the calling thread is inside a GC critical section
        // (mutable construction + CAS-into-root, see ProtoObject::setAttribute
        // and SparseList::implSetAt), do NOT park.  See safepoint() for the
        // rationale — STW root collection during a half-built structure
        // would orphan the in-flight cells.
        if (this && this->space &&
            (allocatedCellsCount & 63) == 0 &&
            this->space->stwFlag.load(std::memory_order_relaxed) &&
            std::this_thread::get_id() != this->space->gcThread->get_id()
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
            && this->criticalSectionDepth == 0
#endif
            ) {
            this->space->parkedThreads++;
            {
                GC_LOCK_TRACE("allocCell STW ACQ");
                std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                this->space->gcCV.notify_all();
                this->space->stopTheWorldCV.wait(lock, [this] { return !this->space->stwFlag.load(); });
                GC_LOCK_TRACE("allocCell STW ACQ(wake)");
                GC_LOCK_TRACE("allocCell STW REL");
            }
            this->space->parkedThreads--;
        }

        Cell* newCell = nullptr;
        if (this && this->thread) {
            newCell = toImpl<ProtoThreadImplementation>(this->thread)->implAllocCell(this);
        } else if (this) {
            while (lock.test_and_set(std::memory_order_acquire)) {}
            // Local context pool check
            if (this->freeCells) {
                newCell = this->freeCells;
                this->freeCells = newCell->getNext();
            } else if (this->space) {
                newCell = this->space->getFreeCells(nullptr);
                if (newCell) {
                    this->freeCells = newCell->getNext();
                }
            }
            lock.clear(std::memory_order_release);
        } else {
             // Absolute fall back (rare or error)
             int result = posix_memalign(reinterpret_cast<void**>(&newCell), 64, sizeof(BigCell));
             if (result != 0) return nullptr;
        }

        if (newCell) {
            std::memset(newCell, 0, 64);
            if (this) {
                this->allocatedCellsCount++;
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                // Per-context allocation-threshold submission.
                //
                // When this context has accumulated more than the threshold
                // since its last submission, hand the young chain over to
                // dirtySegments so the next GC cycle can reclaim the
                // unreachable cells.  We do NOT trigger a GC here — the
                // freeRatio gate inside getFreeCells already kicks the
                // collector when it actually matters.
                //
                // This is safe even when a native helper (mutable
                // setAttribute, SparseList rebuild, ...) is mid-construction
                // because such helpers wrap their construct + CAS sequence
                // in a critical section; the per-thread STW poll skips
                // parking while the depth is non-zero, so a chain submitted
                // here cannot become a sweep candidate before the helper's
                // CAS makes it reachable from a GC root.
                // Per-context allocation-threshold submission moved to
                // ProtoContext::safepoint() so it can only land at points
                // the embedder considers safe (between bytecode opcodes,
                // for example).  Submitting from inside allocCell mid-
                // construction races with native helpers that hold cell
                // pointers in C++ locals across allocations.
#endif
            }
            return newCell;
        }
        else
        {
            if (this && this->space->outOfMemoryCallback)
                (this->space->outOfMemoryCallback(this));
            if (std::getenv("PROTO_ENV_DIAG")) {
                std::cerr << "NO MORE MEMORY!!: " << std::endl;
            }
            exit(-1);
        }
    }

    /**
     * @brief Adds a newly constructed cell to this context's tracking list.
     */
    void ProtoContext::addCell2Context(Cell* cell)
    {
        if (this) {
            while (lock.test_and_set(std::memory_order_acquire)) {}
            cell->setNext(this->lastAllocatedCell);
            this->lastAllocatedCell = cell;
            lock.clear(std::memory_order_release);
        } else {
            cell->setNext(nullptr);
        }
    }

    //=========================================================================
    // Factory Methods
    //=========================================================================

    const ProtoObject* ProtoContext::fromUTF8String(const char* zeroTerminatedUtf8String)
    {
        unsigned int codepoints[INLINE_STRING_MAX_BYTES];
        int count = 0;
        const unsigned char* s = (const unsigned char*)zeroTerminatedUtf8String;
        bool allASCII = true;
        while (*s && count <= INLINE_STRING_MAX_BYTES) {
            unsigned int unicodeChar;
            int len;
            if (*s < 0x80) {
                unicodeChar = *s;
                len = 1;
            } else if ((*s & 0xE0) == 0xC0) {
                unicodeChar = *s & 0x1F;
                len = 2;
            } else if ((*s & 0xF0) == 0xE0) {
                unicodeChar = *s & 0x0F;
                len = 3;
            } else {
                unicodeChar = *s & 0x07;
                len = 4;
            }
            for (int i = 1; i < len; ++i) {
                if (s[i] == '\0' || (s[i] & 0xC0) != 0x80) {
                    unicodeChar = *s;
                    len = 1;
                    break;
                }
                unicodeChar = (unicodeChar << 6) | (s[i] & 0x3F);
            }
            if (count < INLINE_STRING_MAX_BYTES) codepoints[count] = unicodeChar;
            if (unicodeChar >= 128u) allASCII = false;
            ++count;
            s += len;
        }
        if (count > 0 && count <= INLINE_STRING_MAX_BYTES && allASCII && !*s) {
            // ASCII-only path: uses createInlineString (codepoint array).
            // For multi-byte UTF-8 inline strings, use createInlineStringUTF8 directly
            // or go through ProtoString creation which routes via wrapRoot.
            return createInlineString(this, count, codepoints);
        }
        // GC critical section: the loop below builds a ProtoList of
        // codepoints incrementally — each implAppendLast returns a new
        // immutable tree held in `charList` (a C++ local) until the
        // pendingRoot update on the next line.  Subsequent
        // ProtoString::create allocations would otherwise be vulnerable
        // to a sweep landing in the gap between assignment and
        // pendingRoot store.  pendingRoot still acts as the
        // single-cell pin for the charList head; the critical section
        // is the broader guarantee that no STW root scan happens until
        // the whole construction finishes.
        ProtoContext::CriticalSection cs(this);
        const ProtoListImplementation* charList = new(this) ProtoListImplementation(this);
        this->pendingRoot = const_cast<ProtoListImplementation*>(charList);
        s = (const unsigned char*)zeroTerminatedUtf8String;
        while (*s) {
            unsigned int unicodeChar;
            int len;
            if (*s < 0x80) {
                unicodeChar = *s;
                len = 1;
            } else if ((*s & 0xE0) == 0xC0) {
                unicodeChar = *s & 0x1F;
                len = 2;
            } else if ((*s & 0xF0) == 0xE0) {
                unicodeChar = *s & 0x0F;
                len = 3;
            } else {
                unicodeChar = *s & 0x07;
                len = 4;
            }
            for (int i = 1; i < len; ++i) {
                if (s[i] == '\0' || (s[i] & 0xC0) != 0x80) {
                    unicodeChar = *s;
                    len = 1;
                    break;
                }
                unicodeChar = (unicodeChar << 6) | (s[i] & 0x3F);
            }
            charList = charList->implAppendLast(this, fromUnicodeChar(unicodeChar));
            this->pendingRoot = const_cast<ProtoListImplementation*>(charList);
            s += len;
        }
        const ProtoObject* result = ProtoString::create(this, charList->asProtoList(this))->asObject(this);
        this->pendingRoot = nullptr;
        return result;
    }

    const ProtoList* ProtoContext::newList()
    {
        return (new(this) ProtoListImplementation(this, PROTO_NONE, true, nullptr, nullptr))->asProtoList(this);
    }

    const ProtoTuple* ProtoContext::newTuple()
    {
        return (new(this) ProtoTupleImplementation(this, nullptr, 0))->asProtoTuple(this);
    }

    const ProtoTuple* ProtoContext::newTuple(const std::vector<const ProtoObject*>& elements)
    {
        return ProtoTupleImplementation::tupleFromVector(this, elements)->asProtoTuple(this);
    }

    const ProtoTuple* ProtoContext::newTupleFromList(const ProtoList* sourceList)
    {
        return ProtoTupleImplementation::tupleFromList(this, toImpl<const ProtoListImplementation>(sourceList))->asProtoTuple(this);
    }

    const ProtoSparseList* ProtoContext::newSparseList()
    {
        return (new(this) ProtoSparseListImplementation(this, 0, PROTO_NONE, nullptr, nullptr, true))->asSparseList(this);
    }

    const ProtoSet* ProtoContext::newSet()
    {
        // GC critical section: two allocations in a single expression
        // (newSparseList for the empty backing store, then the wrapping
        // ProtoSetImplementation).  Argument-evaluation order is
        // unspecified, so the freshly allocated SparseList is reachable
        // only via a C++ temporary across the SetImplementation alloc.
        ProtoContext::CriticalSection cs(this);
        return (new(this) ProtoSetImplementation(this, newSparseList(), 0))->asProtoSet(this);
    }

    const ProtoMultiset* ProtoContext::newMultiset()
    {
        // GC critical section: same pattern as newSet().
        ProtoContext::CriticalSection cs(this);
        return (new(this) ProtoMultisetImplementation(this, newSparseList(), 0))->asProtoMultiset(this);
    }

    const ProtoByteBuffer* ProtoContext::newByteBuffer(const char* data, unsigned long len)
    {
        // Allocate a fresh buffer owned by the GC descriptor.
        // The implementation accepts `nullptr` and allocates internally.
        char* buf = nullptr;
        if (len > 0) {
            buf = new char[len];
            if (data) std::memcpy(buf, data, len);
            else std::memset(buf, 0, len);
        }
        return (new(this) ProtoByteBufferImplementation(this, buf, len, /*freeOnExit=*/true))
            ->asByteBuffer(this);
    }

    const ProtoObject* ProtoContext::newRangeIterator(long long start, long long stop, long long step)
    {
        return (new(this) ProtoRangeIteratorImplementation(this, start, stop, step))->implAsObject(this);
    }

    const ProtoObject* ProtoContext::newObject(const bool mutableObject)
    {
        unsigned long ref = mutableObject ? generate_mutable_ref(this) : 0;
        // GC critical section: `attributes` is held in a C++ local across
        // the wrapping ProtoObjectCell allocation.  Same discipline as
        // ProtoObject::newChild (which already wraps); this is the
        // simpler factory path used everywhere else.
        ProtoContext::CriticalSection cs(this);
        const auto* attributes = toImpl<const ProtoSparseListImplementation>(newSparseList());
        const ProtoObject* result = (new(this) ProtoObjectCell(this, nullptr, attributes, ref))->asObject(this);
        return result;
    }

    const ProtoObject* ProtoContext::newExternalBuffer(unsigned long size)
    {
        return (new(this) ProtoExternalBufferImplementation(this, size))->implAsObject(this);
    }

    const ProtoObject* ProtoContext::fromBoolean(bool value) {
        return value ? PROTO_TRUE : PROTO_FALSE;
    }

    const ProtoObject* ProtoContext::fromByte(char c) {
        return fromInteger(c);
    }

    const ProtoObject* ProtoContext::fromInteger(long long value) {
        // Check if the value fits within the 54-bit signed integer range
        if (value >= -9007199254740992LL && value <= 9007199254740991LL) {
            ProtoObjectPointer p{};
            p.si.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
            p.si.embedded_type = EMBEDDED_TYPE_SMALLINT;
            p.si.smallInteger = value;
            return p.oid;
        } else {
            // Value is too large for a small integer, create a LargeIntegerImplementation
            return Integer::fromLong(this, value);
        }
    }

    const ProtoObject* ProtoContext::fromLong(long long value) {
        return Integer::fromLong(this, value);
    }

    const ProtoObject* ProtoContext::fromString(const char* str, int base) {
        return Integer::fromString(this, str, base);
    }

    const ProtoObject* ProtoContext::fromDouble(double value) {
        return (new(this) DoubleImplementation(this, value))->implAsObject(this);
    }

    const ProtoObject* ProtoContext::fromMethod(ProtoObject* self, ProtoMethod method) {
        return (new(this) ProtoMethodCell(this, self, method))->implAsObject(this);
    }

    const ProtoObject* ProtoContext::fromExternalPointer(void* pointer, void (*finalizer)(void*)) {
        return (new(this) ProtoExternalPointerImplementation(this, pointer, finalizer))->implAsObject(this);
    }

    const ProtoObject* ProtoContext::fromUnicodeChar(unsigned int unicodeChar) {
        ProtoObjectPointer p{};
        p.unicodeChar.pointer_tag = POINTER_TAG_EMBEDDED_VALUE;
        p.unicodeChar.embedded_type = EMBEDDED_TYPE_UNICODE_CHAR;
        p.unicodeChar.unicodeValue = unicodeChar;
        return p.oid;
    }

    const ProtoObject* ProtoContext::fromBuffer(unsigned long length, char* buffer, bool freeOnExit) {
        return (new(this) ProtoByteBufferImplementation(this, buffer, length, freeOnExit))->implAsObject(this);
    }

    const ProtoObject* ProtoContext::newBuffer(unsigned long length) {
        return (new(this) ProtoByteBufferImplementation(this, nullptr, length, true))->implAsObject(this);
    }

    ReturnReference::ReturnReference(ProtoContext* context, Cell* rv) : Cell(context), returnValue(rv) {}

    const ProtoObject* ReturnReference::implAsObject(ProtoContext* context) const
    {
        // A ReturnReference should not be exposed as a user-facing object.
        return PROTO_NONE;
    }

    void ReturnReference::processReferences(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const Cell*)) const
    {
        if (returnValue && ProtoObject::isCellPointer(reinterpret_cast<const ProtoObject*>(returnValue))) {
            method(context, self, ProtoObject::asCellPointer(reinterpret_cast<const ProtoObject*>(returnValue)));
        }
    }

} // namespace proto
