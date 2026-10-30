/*
 * ProtoSpace.cpp
 *
 *  Created on: 2020-05-23
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include "ModuleCache.h"
#include "AdaptiveHeap.h"
#include "Sweep.h"
#include "MarkStack.h"
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <set>
#include <vector>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <thread>
#include <cstring>
#if defined(__linux__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace proto {

    namespace {
        // This OS thread's walk-pin shard, plus one; 0 until its first walk.
        // Constant-initialised, so reading it needs no guard; initial-exec
        // where the compiler supports it, so reading it needs no
        // __tls_get_addr call from the shared library (four bytes, well within
        // the static TLS reserve glibc keeps for libraries loaded with
        // dlopen).  An index, not a root set pointer: the same index serves
        // every space of the process.
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((tls_model("initial-exec")))
#endif
        thread_local unsigned tlsWalkPinShard = 0;
        std::atomic<unsigned> nextWalkPinShard{0};
    }

    namespace {
        // The calling ProtoThread's own walk-pin set and the space it belongs
        // to; null until the thread's first walk in its home space, null
        // again after releaseThreadWalkPins.
        thread_local ProtoSpace* tlOwnWalkPinSpace = nullptr;
        thread_local ProtoRootSet* tlOwnWalkPins = nullptr;

        // The space whose main-thread walk-pin set this OS thread owns -- the
        // last space it constructed -- and that space's id.  Thread-local
        // storage, not ProtoSpace::mainThreadId: an OS thread id can be reused
        // once the thread that constructed the space has exited, and a new
        // thread with the old id would then write a single-writer set without
        // being counted in the space's quorum.  This storage dies with its
        // thread.  The id (never reused) guards a space constructed later at
        // the address of a destroyed one.
        thread_local const ProtoSpace* tlMainWalkPinSpace = nullptr;
        thread_local proto_ulong tlMainWalkPinSpaceId = 0;
    }

    ProtoRootSet* ProtoSpaceImplementation::walkPinSetFor(ProtoSpace* space) {
        const ProtoSpaceImplementation* impl = space->impl;
        if (impl->singleWriterWalkPins) {
            if (tlOwnWalkPinSpace == space) return tlOwnWalkPins;
            if (tlMainWalkPinSpace == space && tlMainWalkPinSpaceId == spaceIdOf(space))
                return impl->mainThreadWalkPins;
            if (!tlOwnWalkPinSpace && multispace::isWorkerOf(space)) {
                // First walk of a ProtoThread in its home space: once per thread.
                tlOwnWalkPins = createSingleWriterRootSet(space, "protoCore-thread-walk-pins");
                tlOwnWalkPinSpace = space;
                return tlOwnWalkPins;
            }
        }
        return impl->walkPins[walkPinShard()];
    }

    void releaseThreadWalkPins(ProtoSpace* space) {
        if (tlOwnWalkPinSpace != space || !tlOwnWalkPins) return;
        space->destroyRootSet(tlOwnWalkPins);
        tlOwnWalkPins = nullptr;
        tlOwnWalkPinSpace = nullptr;
    }

    unsigned ProtoSpaceImplementation::walkPinShard() {
        unsigned s = tlsWalkPinShard;
        if (s == 0) {
            // Wraps harmlessly: 2^32 is a multiple of kWalkPinShards.
            s = nextWalkPinShard.fetch_add(1, std::memory_order_relaxed) % kWalkPinShards + 1;
            tlsWalkPinShard = s;
        }
        return s - 1;
    }

#ifdef PROTOCORE_GC_INSTRUMENT
    namespace gcprof {
        std::atomic<std::uint64_t> mutatorParkNs{0};
        std::atomic<std::uint64_t> mutatorParks{0};
        std::atomic<std::uint64_t> headroomWaitNs{0};
        std::atomic<std::uint64_t> headroomWaits{0};
        std::atomic<std::uint64_t> refillFreshCells{0};
        std::atomic<std::uint64_t> refillRecycledCells{0};
        // Free chunks carved from a fresh OS block (globalMutex held): a pop
        // from one of these hands out fresh, contiguous cells.
        std::unordered_set<const void*>& freshChunks() {
            static auto* set = new std::unordered_set<const void*>();
            return *set;
        }
    }
#endif

    namespace sweep {
        // Acquire a FreeChunk struct (from pool, or fresh allocation).
        // Caller holds globalMutex.
        ProtoSpace::FreeChunk* takeFreeChunk(ProtoSpace* space) {
            if (space->freeChunkPool) {
                ProtoSpace::FreeChunk* c = space->freeChunkPool;
                space->freeChunkPool = c->next;
                return c;
            }
            return new ProtoSpace::FreeChunk();
        }

        // Return a FreeChunk struct to the pool for reuse.  Caller holds globalMutex.
        void recycleFreeChunk(ProtoSpace* space, ProtoSpace::FreeChunk* chunk) {
            chunk->head = nullptr;
            chunk->tail = nullptr;
            chunk->count = 0;
            chunk->next = space->freeChunkPool;
            space->freeChunkPool = chunk;
        }

        // Publish an accumulated chunk of dead cells to the global freeChunks
        // list.  Caller MUST hold globalMutex.  The chunk's tail must already
        // have its `next` pointing at nullptr (terminator).
        void publishFreeChunk(ProtoSpace* space, Cell* head, Cell* tail, proto_ulong count) {
            if (!head || !tail || count == 0) return;
            tail->internalSetNextRaw(nullptr);
            ProtoSpace::FreeChunk* chunk = takeFreeChunk(space);
            chunk->head = head;
            chunk->tail = tail;
            chunk->count = count;
            chunk->next = space->freeChunks;
            space->freeChunks = chunk;
            // freeCellsCount is an int field of ProtoSpace (its layout is part
            // of the ABI).  It counts cells of one heap; 2^31 cells would be
            // 128 GiB, so a chunk's count fits it exactly.
            relaxedFetchAdd(space->freeCellsCount, static_cast<int>(count));
            // A thread waiting for headroom takes these cells now, not at
            // its 50 ms watchdog (a no-op when no thread waits).
            adaptive::cellsPublished(space);
        }

    }  // namespace sweep

    // The perennial empty root of each shard of the global mutable table (see
    // retainedHeapBlocks in the namespace below for why retained memory must
    // stay reachable).  External linkage on purpose: the array is only ever
    // written, and an internal one is removed by the optimiser together with
    // its stores.  Written under globalMutex, once per shard.
    extern const ProtoSparseList* initialMutableShardRoots[ProtoSpace::MUTABLE_ROOT_SHARDS];
    const ProtoSparseList* initialMutableShardRoots[ProtoSpace::MUTABLE_ROOT_SHARDS] = {};

    namespace {
        /** Maximum bytes to request from the OS in a single getFreeCells allocation (16 MiB). */
        constexpr proto_ulong kMaxBytesPerOSAllocation = 16u * 1024u * 1024u;
        /** Maximum number of blocks (BigCells) per OS request. */
        constexpr int kMaxBlocksPerOSAllocation = static_cast<int>(kMaxBytesPerOSAllocation / sizeof(BigCell));

        /**
         * The base address of every OS block of every space's cell heap.
         *
         * protoCore never returns cell memory to the system, not even when a
         * space is destroyed (docs/MemoryModel.md section 1): cells of a dead
         * space may still be reached from the process-wide tables (a module
         * another space adopted, for one).  Until 2.14.5 the base pointers
         * were dropped once the block was chained into cells, so a destroyed
         * space left blocks that nothing in the process pointed at, and
         * LeakSanitizer reported them at exit.  Recording them states the
         * retention in the code: the blocks are reachable for the life of
         * the process, which is what the design says they are.  One push per
         * OS block (at most 16 MiB of cells), under globalMutex.
         *
         * Never destroyed, like the other process-wide structures a collector
         * reaches: a process may exit while a collector grows a heap.
         */
        std::vector<void*>& retainedHeapBlocks() {   // guarded by globalMutex
            static auto* blocks = new std::vector<void*>();
            return *blocks;
        }

        /**
         * The perennial empty list each shard of the global mutable table
         * starts with.  The first mutable object stored in a shard replaces
         * its root with a heap tree, and nothing referenced the perennial
         * empty any more: 256 cells per process that LeakSanitizer reported
         * at exit.  Perennial means reachable for the life of the process, so
         * they are held in initialMutableShardRoots (below, outside this
         * namespace).
         */
        /**
         * Under a hard heap limit, all running threads' refill batches
         * together use at most maxHeapSize / kLimitBatchFraction cells: each
         * refill is capped at maxHeapSize / (kLimitBatchFraction * threads).
         * Cells a thread holds in its private freelist count against the
         * limit but cannot be reclaimed, so this bounds the part of the limit
         * that is out of the collector's reach.
         */
        constexpr proto_long kLimitBatchFraction = 8;
        /**
         * Floor of a capped refill, so that a very small limit or many threads
         * do not turn every few allocations into a refill under globalMutex.
         */
        constexpr proto_long kMinLimitedBatchCells = 512;

        std::atomic<uint64_t> s_getFreeCellsCalls{0};
        static long long diagCurrentTid() {
#if defined(__linux__)
            return static_cast<long long>(syscall(SYS_gettid));
#else
            return static_cast<long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
        }
        std::mutex s_diagTidsMutex;
        std::unordered_set<long long> s_diagTids;
        void diagPrintAllocCount() {
            (void)s_getFreeCellsCalls;
            (void)s_diagTidsMutex;
            (void)s_diagTids;
        }
        const ProtoList* buildDefaultResolutionChain(ProtoContext* ctx) {
            // GC critical section: `chain` accumulates entries across the
            // loop and is held in this C++ local between iterations; each
            // appendLast also enters its own critical section internally,
            // but the outer guard covers the gap between them where
            // chain would otherwise be unrooted.
            ProtoContext::CriticalSection cs(ctx);
            const ProtoList* chain = ctx->newList();
            if (!chain) return nullptr;
#if defined(_WIN32)
            const char* defaults[] = { ".", "C:\\Program Files\\proto\\lib" };
            const int n = 2;
#elif defined(__APPLE__)
            const char* defaults[] = { ".", "/usr/local/lib/proto" };
            const int n = 2;
#else
            const char* defaults[] = { ".", "/usr/lib/proto", "/usr/local/lib/proto" };
            const int n = 3;
#endif
            for (int i = 0; i < n && chain; ++i) {
                const ProtoObject* s = ctx->fromUTF8String(defaults[i]);
                if (s) chain = chain->appendLast(ctx, s);
            }
            return chain;
        }

        using sweep::publishFreeChunk;
        using sweep::recycleFreeChunk;

        const char* cellTypeName(CellType type) {
            switch (type) {
                case CellType::None: return "None";
                case CellType::Object: return "Object";
                case CellType::List: return "List";
                case CellType::Tuple: return "Tuple";
                case CellType::String: return "String";
                case CellType::SparseList: return "SparseList";
                case CellType::Method: return "Method";
                case CellType::ExternalPointer: return "ExternalPointer";
                case CellType::ExternalBuffer: return "ExternalBuffer";
                case CellType::Thread: return "Thread";
                case CellType::LargeInteger: return "LargeInteger";
                case CellType::Double: return "Double";
                case CellType::Set: return "Set";
                case CellType::Multiset: return "Multiset";
                case CellType::MethodCell: return "MethodCell";
                case CellType::SparseListIterator: return "SparseListIterator";
                case CellType::ListIterator: return "ListIterator";
                case CellType::TupleIterator: return "TupleIterator";
                case CellType::StringIterator: return "StringIterator";
                case CellType::RangeIterator: return "RangeIterator";
                case CellType::SetIterator: return "SetIterator";
                case CellType::MultisetIterator: return "MultisetIterator";
                case CellType::ReturnReference: return "ReturnReference";
                case CellType::ParentLink: return "ParentLink";
                case CellType::ThreadExtension: return "ThreadExtension";
                case CellType::ByteBuffer: return "ByteBuffer";
                case CellType::TupleDictionary: return "TupleDictionary";
                case CellType::StringLeafNode: return "StringLeafNode";
                case CellType::StringInternalNode: return "StringInternalNode";
                case CellType::ListSmall: return "ListSmall";
                case CellType::SparseListSmall: return "SparseListSmall";
                case CellType::Map: return "Map";
                case CellType::MapSmall: return "MapSmall";
                case CellType::MapIterator: return "MapIterator";
                case CellType::MPSCQueue: return "MPSCQueue";
                case CellType::MPSCQueueNode: return "MPSCQueueNode";
                case CellType::MPSCQueueRetain: return "MPSCQueueRetain";
                case CellType::LinkedList: return "LinkedList";
            }
            return "unknown";
        }

        // Check a child that `parent->processReferences` reported.  Called
        // from the GC thread only.
        //
        // A reported child is never nullptr: every implementation loads each
        // reference field once and reports it only when
        // ProtoObject::asCellPointer yields a cell.  A null here therefore
        // means a broken implementation.  Instrumented and debug builds stop
        // and name the reporting cell's type, so the offender is identified
        // instead of masked; other builds let it through, and
        // Marker::shade skips null entries.
        inline void checkReportedReference(const Cell* parent,
                                           const Cell* ref,
                                           const char* phase) {
            if (reinterpret_cast<uintptr_t>(ref) & 1) {
                std::cerr << "CRITICAL TAGGED POINTER (" << phase << "): " << ref
                          << " from parent " << parent
                          << " type " << (int)parent->getType() << std::endl;
                std::abort();
            }
#if defined(PROTOCORE_GC_INSTRUMENT) || !defined(NDEBUG)
            if (!ref) {
                std::fprintf(stderr,
                    "protoCore GC: null reference reported by a cell of type %d (%s) at %p during %s\n",
                    static_cast<int>(parent->getType()), cellTypeName(parent->getType()),
                    static_cast<void*>(const_cast<Cell*>(parent)), phase);
                std::fflush(stderr);
                std::abort();
            }
#endif
        }

        // The states of one cycle's snapshot of the global mutable table,
        // by ref: an open-addressing table built once per cycle, before the
        // trace, by one walk of the snapshot's shard trees (indexSnapshot),
        // so the handle -> state edge costs one probe instead of a descent
        // of a shard tree per live handle.  The walk reads keys and value
        // pointers and never dereferences a value: an entry whose handle is
        // dead may name a state this cycle frees.  Owned by the collector
        // thread and reused by every cycle (the capacity is kept).
        struct MutableStateIndex {
            struct Slot { proto_ulong ref; const ProtoObject* state; };
            std::vector<Slot> slots;
            std::size_t mask = 0;

            static std::size_t hashOf(proto_ulong ref) {
                return static_cast<std::size_t>((ref * 0x9E3779B97F4A7C15ULL) >> 17);
            }
            void reset(std::size_t entries) {
                std::size_t cap = 16;
                while (cap < 2 * entries + 16) cap <<= 1;
                if (slots.size() < cap) slots.resize(cap);
                std::fill(slots.begin(), slots.begin() + cap, Slot{0, nullptr});
                mask = cap - 1;
            }
            void insert(proto_ulong ref, const ProtoObject* state) {
                std::size_t i = hashOf(ref) & mask;
                while (slots[i].ref != 0 && slots[i].ref != ref) i = (i + 1) & mask;
                slots[i] = Slot{ref, state};
            }
            void prefetch(proto_ulong ref) const { PROTO_PREFETCH(&slots[hashOf(ref) & mask]); }
            const ProtoObject* find(proto_ulong ref) const {
                std::size_t i = hashOf(ref) & mask;
                for (;;) {
                    const Slot& sl = slots[i];
                    if (sl.ref == ref) return sl.state;
                    if (sl.ref == 0) return nullptr;
                    i = (i + 1) & mask;
                }
            }
        };

        // The mark phase's tracer (docs/GarbageCollector.md, "Mark work
        // list").  A cell turns grey when it is shaded: it is marked, added
        // to the marked list and pushed on the mark stack, all at once, so
        // the stack never holds a cell twice.  drain() pops grey cells and
        // shades their references until the stack is empty.
        //
        // The stack has a fixed capacity.  A shade that finds it full leaves
        // the cell white (unmarked) and raises `overflow`; the cell is
        // reachable from a grey or black cell, or from a root, a young cell
        // or a pen cell, and the rescan in gcThreadLoop shades it from
        // there.  Every pass that overflows has marked a full stack of new
        // cells first, so the rescans end.
        //
        // Only the GC thread touches mark bits (see the Phase 3 comment in
        // gcThreadLoop), so marking at push time instead of at pop time
        // changes the order in which cells are marked and nothing else: a
        // cycle marks the same set.
        struct Marker {
            gcmark::MarkStack& stack;
            gcmark::CellBlockList& marked;
            ProtoContext* context;
            bool overflow = false;
            const Cell* parent = nullptr;
            const char* phase = "";

            inline void shade(const Cell* cell) {
                if (!cell || cell->isMarked()) return;
                if (!stack.push(cell)) {
                    overflow = true;
                    return;
                }
                const_cast<Cell*>(cell)->mark();
                marked.push_back(cell);
                // A handle's index slot is read when the handle is scanned:
                // start the miss now, while the cell's line is in cache.
                if (mutableStates && cell->getType() == CellType::Object) {
                    const proto_ulong ref = static_cast<const ProtoObjectCell*>(cell)->mutable_ref;
                    if (ref) mutableStates->prefetch(ref);
                }
            }

            // The references one scan reports are shaded in batches: each is
            // prefetched when reported and shaded when the batch is full or
            // the scan ends, so the misses on their headers (shade reads the
            // mark bit) overlap instead of following one another.
            static constexpr int kBatch = 8;
            const Cell* batch[kBatch];
            int batched = 0;

            inline void flush() {
                for (int i = 0; i < batched; ++i) shade(batch[i]);
                batched = 0;
            }

            static void reportReference(ProtoContext*, void* self, const Cell* ref) {
                auto* m = static_cast<Marker*>(self);
                checkReportedReference(m->parent, ref, m->phase);
                if (!ref) return;
                if ((reinterpret_cast<uintptr_t>(ref) & 0x3F) == 0) PROTO_PREFETCH(ref);
                m->batch[m->batched++] = ref;
                if (m->batched == kBatch) m->flush();
            }

            // The states of the pause's snapshot of the mutable table, by
            // ref (MutableStateIndex), or nullptr when the table's states
            // are roots of the cycle instead (PROTOCORE_GC_REINCLUDE_SURVIVORS
            // off).
            const MutableStateIndex* mutableStates = nullptr;

            // The edge from a mutable object's handle to its current state
            // (docs/GLOBAL_MUTABLE_TABLE.md, "States are not roots").  The
            // handle cell holds only its integer ref; the state is the
            // entry of that ref in the snapshot taken at the pause, never
            // the live table.  A handle written after the pause has no
            // entry in the snapshot: it is young, and so are its new states.
            // The snapshot graph is immutable, so this is an ordinary edge
            // of the frozen graph the concurrent mark walks.
            inline void reportMutableState(const Cell* cell) {
                if (!mutableStates || cell->getType() != CellType::Object) return;
                const proto_ulong ref = static_cast<const ProtoObjectCell*>(cell)->mutable_ref;
                if (ref == 0) return;
                const ProtoObject* state = mutableStates->find(ref);
                if (state && ProtoObject::isCellPointer(state))
                    reportReference(context, this, ProtoObject::asCellPointer(state));
            }

            // Shade every reference `cell` holds.  `cell` itself is not
            // shaded: it is grey already, or a young or pen cell, which is
            // not a candidate of this cycle.
            inline void scan(const Cell* cell, const char* scanPhase) {
                parent = cell;
                phase = scanPhase;
                cell->processReferences(context, this, &Marker::reportReference);
                reportMutableState(cell);
                flush();
            }

            // Marks a cell without scanning it (the table's own nodes, and
            // the emptied states of retireDeadStates).
            inline void markOnly(const Cell* cell) {
                if (cell->isMarked()) return;
                const_cast<Cell*>(cell)->mark();
                marked.push_back(cell);
            }

            void drain() {
                while (!stack.empty()) {
                    const Cell* cell = stack.pop();
                    // Prefetch the next cell to be popped: its fields are
                    // read by the next scan, so the miss overlaps this one.
                    if (!stack.empty()) {
                        const Cell* next = stack.top();
                        if ((reinterpret_cast<uintptr_t>(next) & 0x3F) == 0) {
                            PROTO_PREFETCH(next);
                        }
                    }
                    scan(cell, "Phase4(mark)");
                }
            }
        };

        // Before the trace: marks every node of the snapshot's shard trees,
        // without scanning it, and indexes their entries by ref, reading keys
        // and value pointers only.  A state is then marked only through its
        // handle (Marker::reportMutableState), never through the table.
        // Marking the nodes first stops every other path into the snapshot
        // at its nodes: a young path copy of a shard tree (an older version
        // in a live context's young generation) is scanned by the young walk,
        // and its children in the snapshot are already marked, so the walk
        // does not trace the shard's subtree and its values.  Returns the
        // nodes marked; they are the first entries of the marked list, and
        // the overflow rescans skip them (a scan would shade every state).
        std::size_t indexSnapshot(Marker& marker, MutableStateIndex& index,
                                  ProtoSparseList* const* snapshot,
                                  std::vector<const ProtoSparseListImplementation*>& stack) {
            std::size_t entries = 0;
            for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
                const ProtoSparseList* root = snapshot[s];
                if (!root) continue;
                ProtoObjectPointer p{};
                p.oid = reinterpret_cast<const ProtoObject*>(root);
                if (p.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL) entries += ProtoSparseListSmallImplementation::MAX_INLINE;
                else entries += toImpl<const ProtoSparseListImplementation>(root)->size;
            }
            index.reset(entries);
            const std::size_t markedBefore = marker.marked.size();
            for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
                const ProtoSparseList* root = snapshot[s];
                if (!root) continue;
                ProtoObjectPointer p{};
                p.oid = reinterpret_cast<const ProtoObject*>(root);
                if (p.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL) {
                    const auto* small = toImpl<const ProtoSparseListSmallImplementation>(root);
                    marker.markOnly(small);
                    for (unsigned i = 0; i < ProtoSparseListSmallImplementation::MAX_INLINE; ++i)
                        if (small->keys[i] != 0 && small->values[i]) index.insert(small->keys[i], small->values[i]);
                    continue;
                }
                stack.clear();
                stack.push_back(toImpl<const ProtoSparseListImplementation>(root));
                while (!stack.empty()) {
                    const ProtoSparseListImplementation* n = stack.back();
                    stack.pop_back();
                    if (n->isMarked()) continue;
                    marker.markOnly(n);
                    if (!n->isEmpty && n->value) index.insert(n->key, n->value);
                    if (n->previous) stack.push_back(n->previous);
                    if (n->next) stack.push_back(n->next);
                }
            }
            return marker.marked.size() - markedBefore;
        }

        // After the trace has closed: keeps the states of the snapshot whose
        // handles the trace did not reach, to be emptied after the sweep
        // (emptyRetiredStates; docs/GLOBAL_MUTABLE_TABLE.md, "Dead states
        // become tombstones").
        //
        // Such a state's table node lives on: it stays in the live table
        // until Phase 5b, and the shard-tree versions the mutators build
        // meanwhile share it or copy its value.  Those versions are young
        // cells, which the next cycle's young walk scans.  A state this sweep
        // freed would then be read, marked and scanned through them as if it
        // were live -- freed memory, possibly reused already.  (Measured
        // before this step: handles finalized in one cycle were shaded again
        // in the next through such paths, and the reused cells they reached
        // kept growing garbage alive; protoJS
        // promise_rejections_bounded_memory ran out of memory.)
        //
        // So the state cell itself is marked, without being scanned: what it
        // holds is not kept, and a dead chain of mutable objects is still
        // freed in one cycle.  Only states of this space are kept; entries of
        // destroyed spaces are released by Phase 5b without being read.
        void retireDeadStates(Marker& marker, const MutableStateIndex& index,
                              proto_ulong spaceId, std::vector<const Cell*>& retired) {
            retired.clear();
            for (std::size_t i = 0; i <= index.mask; ++i) {
                const MutableStateIndex::Slot& slot = index.slots[i];
                if (slot.ref == 0 || !slot.state) continue;
                if ((slot.ref >> kMutableRefSpaceShift) != spaceId) continue;
                if (!ProtoObject::isCellPointer(slot.state)) continue;
                const Cell* cell = ProtoObject::asCellPointer(slot.state);
                if (cell->isMarked()) continue;
                // Every state is a plain object cell (the writers in
                // core/ProtoObject.cpp build them); anything else is kept
                // whole, as a root would keep it, rather than emptied.
                if (cell->getType() != CellType::Object ||
                    static_cast<const ProtoObjectCell*>(cell)->asLazyCell() != nullptr) {
                    marker.shade(cell);
                    marker.drain();
                    continue;
                }
                marker.markOnly(cell);
                retired.push_back(cell);
            }
        }

        // After the sweep: empties the states retireDeadStates kept that this
        // cycle's sweep found among its candidates.  The sweep unmarks every
        // candidate it keeps, so such a state is unmarked now; a state still
        // marked was not a candidate (a young cell, or a survivor the cycle
        // only scanned) and was not going to be freed, so it is left whole.
        // An emptied state holds no reference: a later scan of it reports
        // nothing, and it is freed once no scanned path reaches it.  No
        // mutator sees the change: a state is reached only through its
        // handle, and its handle was not reached.  Runs before the cycle ends,
        // so no mark reads an emptied state's former references.
        std::size_t emptyRetiredStates(const std::vector<const Cell*>& retired) {
            std::size_t emptied = 0;
            for (const Cell* cell : retired) {
                if (cell->isMarked()) continue;
                auto* state = const_cast<ProtoObjectCell*>(static_cast<const ProtoObjectCell*>(cell));
                state->parent = nullptr;
                state->attributes = nullptr;
                ++emptied;
            }
            return emptied;
        }

        // Phase 5b: removes from the mutables tree the entries of the mutable
        // objects whose handles this cycle's sweep finalized.  Runs on the GC
        // thread after sweep, unlocked, concurrently with the mutators.
        //
        // ProtoObjectCell::finalize only records a collected handle's
        // mutable_ref in space->gcFinalizedMutableRefs; finalizers never
        // allocate or publish.  Here the refs are grouped by shard.  For each
        // shard the live root is loaded, every recorded ref present in it is
        // removed, and the result is published with one compare-and-swap.  A
        // failed CAS means a mutator published to the shard meanwhile: the
        // root is reloaded and the shard's batch redone from it, so neither
        // side loses an update.  A ref that is absent (a mutable never
        // written) costs a probe and no allocation.
        //
        // Soundness (docs/GarbageCollector.md § "Phase 5b"):
        //  * an unmarked handle is unreachable, and exactly one cell carries
        //    a given mutable_ref (fresh values from nextMutableRef, never
        //    reused; states carry 0), so no live object uses a removed entry;
        //  * mark has finished: a finalized handle was not marked, so its
        //    state was not marked through it either (states are not roots,
        //    docs/GLOBAL_MUTABLE_TABLE.md) and this cycle's sweep freed it
        //    unless something else reached it; the entry is removed without
        //    reading its value;
        //  * a root loaded after the stop-the-world is marked or young, never
        //    a candidate of this cycle, and only this thread frees cells, so
        //    a pending CAS cannot suffer ABA.
        //
        // Path copies are allocated through space->gcContext.  No root scan
        // can observe them half-built, because the GC thread is the only
        // thread that starts a stop-the-world; the context's young generation
        // is submitted at the end, so they are candidates of the next cycle.
        void releaseFinalizedMutableEntries(ProtoSpace* space) {
            std::vector<proto_ulong>& refs = space->gcFinalizedMutableRefs;
            // The entries of spaces destroyed since the last cycle of any
            // space go with them: nothing can reach those handles any more.
            //
            // Except this space's own.  ~ProtoSpace records the space as
            // destroyed before it joins the collector, so a cycle in progress
            // at destruction used to find its own id here and, in its Phase
            // 5b, walk the whole table and remove every live entry of the
            // space -- O(table), at exit, while the destructor waited (a
            // runtime measured ~30 % of a 90 s shutdown here).  The id stays
            // recorded instead and the next cycle of a live space purges those
            // entries, exactly as when the space is destroyed with its
            // collector idle; this cycle still removes the entries of the
            // handles its own sweep finalized, as every cycle does.
            bool ending;
            {
                std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
                ending = space->state == SPACE_STATE_ENDING;
            }
            const proto_ulong ownId = spaceIdOf(space);
            multispace::appendRefsOfDestroyedSpaces(refs, ending ? &ownId : nullptr);
            if (refs.empty()) return;
            ProtoContext* gc = space->gcContext;
            constexpr proto_ulong kShards = ProtoSpace::MUTABLE_ROOT_SHARDS;

            std::sort(refs.begin(), refs.end(), [](proto_ulong a, proto_ulong b) {
                const proto_ulong sa = a % kShards;
                const proto_ulong sb = b % kShards;
                return sa != sb ? sa < sb : a < b;
            });
            // Sorted by shard, then by key: each shard's run is the sorted,
            // duplicate-free key array sparseListRemoveSorted takes.
            refs.erase(std::unique(refs.begin(), refs.end()), refs.end());

            std::size_t begin = 0;
            while (begin < refs.size()) {
                const proto_ulong shard = refs[begin] % kShards;
                std::size_t end = begin + 1;
                while (end < refs.size() && refs[end] % kShards == shard) ++end;

                auto& slot = globalMutableShards[shard].root;
                for (int attempt = 1;; ++attempt) {
                    ProtoSparseList* oldRoot = slot.load(std::memory_order_acquire);
                    if (!oldRoot) break;
                    // One pass for the whole run. A removeAt per entry
                    // path-copied the shard once per dead mutable: garbage
                    // the collector allocates past the heap ceiling, since it
                    // cannot wait for its own cycle (SparseListBulkRemove, protoST cli_memory_bounded).
                    const ProtoSparseList* newRoot =
                        sparseListRemoveSorted(gc, oldRoot, refs.data() + begin, end - begin);
                    if (newRoot == oldRoot) break;  // none of the refs is present
                    ProtoSparseList* expected = oldRoot;
                    if (slot.compare_exchange_strong(expected, const_cast<ProtoSparseList*>(newRoot),
                                                     std::memory_order_acq_rel)) {
                        break;
                    }
                    // A mutator published to this shard: redo the batch from
                    // the new root, yielding now and then like the writers.
                    if ((attempt & 31) == 0) std::this_thread::yield();
                }
                begin = end;
            }
            refs.clear();  // keeps the capacity for the next cycle

            // Hand the path copies, including those of lost attempts, to the
            // collector: the same handover as the threshold submission in
            // ProtoContext::safepoint.  `gc` is this thread's own context.
            Cell* chain = gc->lastAllocatedCell;
            gc->lastAllocatedCell = nullptr;
            gc->allocatedCellsCount = 0;
            if (chain) space->submitYoungGeneration(chain);
        }

        void gcThreadLoop(ProtoSpace* space) {
            // The mark stack and the marked list live as long as the
            // collector thread and are reused by every cycle (MarkStack.h).
            gcmark::MarkStack markStack(gcmark::markStackCapacity());
            gcmark::CellBlockList markedList;
            MutableStateIndex mutableStates;
            std::vector<const ProtoSparseListImplementation*> tableNodeStack;
            std::vector<const Cell*> retiredStates;
            std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            GC_LOCK_TRACE("gcLoop ACQ(init)");
#ifdef PROTOCORE_GC_INSTRUMENT
            // Per-phase running totals + cycle count, printed every 5
            // cycles when env var PROTOCORE_GC_PROFILE is set.  Compile
            // out by default; enable with cmake -DPROTOCORE_GC_INSTRUMENT=ON.
            static std::atomic<uint64_t> dbg_total_phase1_us{0};
            static std::atomic<uint64_t> dbg_total_phase2_us{0};
            static std::atomic<uint64_t> dbg_total_phase4_us{0};
            static std::atomic<uint64_t> dbg_total_phase5_us{0};
            static std::atomic<uint64_t> dbg_total_phase6_us{0};
            // Phase 5b, the release of mutables-tree entries (REL= in the line).
            static std::atomic<uint64_t> dbg_total_release_us{0};
            static std::atomic<uint64_t> dbg_total_cells_marked{0};
            static std::atomic<uint64_t> dbg_total_segments_swept{0};
            // Finer split (the [GC-PHASES] line).  All cumulative.
            //  token: waiting for the process-wide cycle token (part of P1)
            //  quorum: stwFlag raised -> every mutator parked (part of P1)
            //  stw: every mutator parked -> world resumed (the real pause)
            //  young: the trace from every source (roots, young chains,
            //         survivor pen), interleaved since 2.14.6; before it,
            //         only the young-chain and pen walks
            //  trace: the overflow rescans since 2.14.6; before it, the
            //         transitive mark loop.  Compare young + trace across
            //         versions.
            //  mark_stack_peak / marked_list_max_kb: the largest mark stack
            //         (entries) and marked list (KiB of blocks) of any cycle;
            //         mark_rescans: overflow rescans, cumulative
            //  unmark: bulk unmark of markedList (part of P6)
            //  busy: whole cycle minus token wait
            static uint64_t dbg_ns_token = 0, dbg_ns_quorum = 0, dbg_ns_stw = 0,
                            dbg_ns_young = 0, dbg_ns_trace = 0, dbg_ns_unmark = 0,
                            dbg_ns_busy = 0, dbg_ns_max_stw = 0;
            static uint64_t dbg_young_cells = 0, dbg_swept_cells = 0,
                            dbg_freed_cells = 0, dbg_roots = 0;
            static uint64_t dbg_mark_stack_peak = 0, dbg_mark_rescans = 0,
                            dbg_marked_list_max_bytes = 0;
            auto dbgNs = [](std::chrono::steady_clock::time_point a,
                            std::chrono::steady_clock::time_point b) {
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
            };
            std::chrono::steady_clock::time_point t_token_end, t_young_start, t_trace_start;
            // CPU time of the collector thread itself (Linux): tells a slow
            // phase that ran on the CPU from one that waited for it.
            auto dbgCpuNs = []() -> uint64_t {
#if defined(__linux__)
                struct timespec ts;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
                return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#else
                return 0;
#endif
            };
            static uint64_t dbg_cpu_mark = 0, dbg_cpu_sweep = 0, dbg_cpu_busy = 0;
            uint64_t c_token_end = 0, c_young_start = 0, c_sweep_start = 0;
            const bool dbg_profile = std::getenv("PROTOCORE_GC_PROFILE") != nullptr;
#endif
#if defined(__linux__)
            // A name for profilers (perf --per-thread, top -H); 15 bytes max.
            pthread_setname_np(pthread_self(), "protocore-gc");
#endif
            // The collector takes part in grace periods (it reads the global
            // mutable table in Phase 5b) but is quiescent everywhere else.
            multispace::ensureQuiescenceRecord();
            multispace::setQuiescenceOut(true);
            while (space->state != SPACE_STATE_ENDING) {
                // Wait for a GC trigger or space ending
                space->gcCV.wait(lock, [space] {
                    return space->gcStarted || space->state == SPACE_STATE_ENDING;
                });
                GC_LOCK_TRACE("gcLoop ACQ(gcStarted)");
                if (space->state == SPACE_STATE_ENDING) break;
#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase1_start = std::chrono::steady_clock::now();
#endif
                
                // --- CYCLE TOKEN ---
                //
                // One collection cycle at a time in the process.  Every space
                // marks the global mutable table, so a marker reaches cells of
                // other spaces; two markers at once would share the one mark
                // bit of those cells (docs/GLOBAL_MUTABLE_TABLE.md).  The wait
                // releases globalMutex, so the mutators of this space keep
                // running, and it is uncontended with a single space.
                //
                // It also waits until no cycle that did not root the table's
                // states is still before its Phase 5b
                // (multispace::mutableReleasesPending, raised only by such a
                // cycle).  A cycle of a several-space process roots the
                // states (Phase 2) and reads every value of its snapshot, so
                // its snapshot must not hold an entry whose state another
                // cycle has freed.  A cycle releases before it hands the
                // token on, except with several spaces, where its release
                // follows the grace period; this wait covers a space created
                // during such a cycle.
                multispace::cycleCV().wait(lock, [space] {
                    return (!multispace::cycleActive
                            && multispace::mutableReleasesPending.load(std::memory_order_seq_cst) == 0)
                        || space->state == SPACE_STATE_ENDING;
                });
                if (space->state == SPACE_STATE_ENDING) break;
                multispace::cycleActive = true;
#ifdef PROTOCORE_GC_INSTRUMENT
                t_token_end = std::chrono::steady_clock::now();
                c_token_end = dbgCpuNs();
                dbg_ns_token += dbgNs(t_phase1_start, t_token_end);
#endif
                // The adaptive heap controller's trace reports the cycle's
                // duration from here (a no-op unless it is enabled).
                adaptive::onCycleStart(space);

                // --- PHASE 1: STOP THE WORLD ---
                multispace::noteCycleStart();
                space->stwFlag.store(true);
                // Read by every allocation poll of the process: a thread of
                // another space that belongs to this one parks for it too.
                multispace::attention.fetch_add(1);
                // We need to wait until ALL other threads are parked
                // runningThreads includes all application threads.
                // GC thread doesn't increment/decrement runningThreads.
                // We need to wait until ALL other threads are parked
                // runningThreads includes all application threads.
                // GC thread doesn't increment/decrement runningThreads.
                space->gcCV.wait(lock, [space] {
                    return space->parkedThreads.load() >= space->runningThreads.load() || space->state == SPACE_STATE_ENDING;
                });
                GC_LOCK_TRACE("gcLoop ACQ(parked)");
                // The adaptive heap controller counts the stop-the-world as
                // mutator stall from here, where the world IS stopped, to the
                // resume below.  The time to reach the quorum is not counted:
                // until the last thread parks, that thread is still running.
                const auto stwStart = std::chrono::steady_clock::now();
                if (space->state == SPACE_STATE_ENDING) {
                    // Lower the flag raised above: a thread parked for it, of
                    // this space or of another one it belongs to, would
                    // otherwise wait for ever.
                    space->stwFlag.store(false);
                    multispace::attention.fetch_sub(1);
                    space->stopTheWorldCV.notify_all();
                    multispace::noteCycleEnd();
                    multispace::cycleActive = false;   // globalMutex is held here
                    multispace::cycleCV().notify_all();
                    break;
                }
#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase2_start = std::chrono::steady_clock::now();
                dbg_ns_quorum += dbgNs(t_token_end, stwStart);
                dbg_total_phase1_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase2_start - t_phase1_start).count(),
                    std::memory_order_relaxed);
#endif

                // --- PHASE 2: COLLECT ROOTS ---
                // The roots, shaded by the mark phase after the world
                // resumes.  Bounded by the program's roots, not its heap.
                std::vector<const Cell*> rootList;
                // One root handle per context for its young generation: the
                // chain head.  The chains are walked after the world resumes.
                std::vector<const Cell*> youngChainHeads;
                auto addRootObj = [&](const ProtoObject* obj) {
                    if (ProtoObject::isCellPointer(obj)) {
                        rootList.push_back(ProtoObject::asCellPointer(obj));
                    }
                };

                // The main thread's contexts can be reached twice: through
                // its ProtoThread (space->threads) and as space->mainContext,
                // the start of that same chain or a suffix of it (a context
                // inherits its thread from `previous`).  Scanning it twice
                // doubled its roots in the pause and its young-chain walk.
                bool mainContextScanned = false;
                auto scanContexts = [&](ProtoContext* currentCtx) {
                    while (currentCtx) {
                        if (currentCtx == space->mainContext) mainContextScanned = true;
                        // Roots: Automatic locals
                        for (unsigned int i = 0; i < currentCtx->getAutomaticLocalsCount(); ++i) {
                            addRootObj(currentCtx->getAutomaticLocals()[i]);
                        }
                        // Roots: Closure locals
                        if (currentCtx->closureLocals) {
                            addRootObj(reinterpret_cast<const ProtoObject*>(currentCtx->closureLocals));
                        }
                        // Roots: Return value
                        if (currentCtx->returnValue) {
                            addRootObj(currentCtx->returnValue);
                        }
                        // Roots: the young generation (cells allocated in this
                        // context and not yet submitted).  They are not
                        // candidates of this cycle, because they are not in the
                        // segments captured below, and they are not marked.
                        // Only the chain head is recorded here, O(1) per
                        // context; the concurrent mark walks the chain and
                        // pushes the references of its cells (see the young-
                        // chain walk after the world resumes).  Read without
                        // the context's lock: the world is stopped, and the
                        // owning thread is parked or out of the quorum (see
                        // ProtoContext::addCell2Context).
                        const Cell* youngHead = currentCtx->lastAllocatedCell;
                        if (youngHead) youngChainHeads.push_back(youngHead);

                        currentCtx = currentCtx->previous;
                    }
                };

                // 1. Scan Thread Stacks.  The threads list may now be in
                // either form (Small with ≤ 3 entries — typical for the
                // common 1-3-thread process; AVL otherwise).  Branch on
                // pointer tag and walk inline pairs for Small, AVL nodes
                // otherwise.
                if (space->threads) {
                    const ProtoObject* rootObj = reinterpret_cast<const ProtoObject*>(space->threads);
                    if (ProtoObject::isCellPointer(rootObj)) {
                        ProtoObjectPointer pa{}; pa.oid = rootObj;
                        if (pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL) {
                            const auto* small = toImpl<const ProtoSparseListSmallImplementation>(rootObj);
                            for (unsigned i = 0; i < ProtoSparseListSmallImplementation::MAX_INLINE; ++i) {
                                if (small->keys[i] == 0) continue;
                                const ProtoObject* v = small->values[i];
                                if (v && v->asThread(space->rootContext)) {
                                    scanContexts(toImpl<const ProtoThreadImplementation>(v->asThread(space->rootContext))->context);
                                }
                            }
                        } else {
                            std::vector<const ProtoSparseListImplementation*> stack;
                            stack.push_back(toImpl<const ProtoSparseListImplementation>(rootObj));
                            while (!stack.empty()) {
                                const ProtoSparseListImplementation* node = stack.back();
                                stack.pop_back();
                                if (!node->isEmpty && node->value && node->value->asThread(space->rootContext)) {
                                    scanContexts(toImpl<const ProtoThreadImplementation>(node->value->asThread(space->rootContext))->context);
                                }
                                if (node->previous) stack.push_back(node->previous);
                                if (node->next) stack.push_back(node->next);
                            }
                        }
                    }
                }
                
                // 2. Global Roots
                addRootObj(space->objectPrototype);
                addRootObj(space->booleanPrototype);
                addRootObj(space->unicodeCharPrototype);
                addRootObj(space->listPrototype);
                addRootObj(space->listIteratorPrototype);
                addRootObj(space->tuplePrototype);
                addRootObj(space->tupleIteratorPrototype);
                addRootObj(space->stringPrototype);
                addRootObj(space->stringIteratorPrototype);
                addRootObj(space->sparseListPrototype);
                addRootObj(space->sparseListIteratorPrototype);
                addRootObj(space->mapPrototype);
                // O(1), a global-structure root: the only addition this type
                // makes to the stop-the-world window (PMQ-SPEC section 3.1).
                addRootObj(space->mpscQueuePrototype);
                addRootObj(space->setPrototype);
                addRootObj(space->setIteratorPrototype);
                addRootObj(space->multisetPrototype);
                addRootObj(space->multisetIteratorPrototype);
                addRootObj(space->rangeIteratorPrototype);
                addRootObj(space->smallIntegerPrototype);
                addRootObj(space->largeIntegerPrototype);
                addRootObj(space->floatPrototype);
                addRootObj(space->doublePrototype);
                addRootObj(space->bytePrototype);
                addRootObj(space->nonePrototype);
                addRootObj(space->methodPrototype);
                addRootObj(space->bufferPrototype);
                addRootObj(space->pointerPrototype);
                addRootObj(space->datePrototype);
                addRootObj(space->timestampPrototype);
                addRootObj(space->timedeltaPrototype);
                addRootObj(space->threadPrototype);
                addRootObj(space->rootObject);
                // literalData is a strong Symbol — covered by the SymbolTable sweep below
                if (space->resolutionChain_) addRootObj(space->resolutionChain_->asObject(space->rootContext));

                // Module roots (P3).  The module list is process-global and is a
                // root: a module's contents are ordinary collectable objects, so
                // an unfreed list would not keep them alive.
                //
                // Record only each shard's published entry count here —
                // O(SHARD_COUNT) under stop-the-world, no entry dereferenced —
                // exactly as the tuple interner does below.  Phase 4 pushes the
                // captured entries after the world resumes.
                //
                // This REPLACED a `for (mod : space->moduleRoots) addRootObj(mod)`
                // loop that ran HERE, inside the pause, holding moduleRootsMutex,
                // and was O(modules).  It was the one term in the documented pause
                // profile that scaled with the program, and it was missing from
                // the cost table.  Do not put a per-module loop back into this
                // window.
                globalModuleRootTable().captureForGC();

                // Tuple interner.  Interned tuples are perennial and the
                // table is a root.  Record only each shard's published
                // entry count here — O(shards) under STW; mark (Phase 4)
                // walks those entries outside the STW window while
                // mutators keep appending.  See TupleInterner.
                if (space->tupleInterner) space->tupleInterner->captureForGC();

                // stringInternMap is intentionally NOT scanned.  The map
                // is held empty for the lifetime of the process:
                // internString() is dead code (never called from any
                // current path — see core/ProtoString.cpp lines 290 and
                // 1195 for why interning was abandoned), and the
                // canonical symbol table is SymbolTable, whose entries
                // are perennial and also not scanned (see comment
                // above).  Scanning an always-empty set was pure GC
                // overhead.  The map infrastructure is left in place
                // (no ABI change) but never iterated.

                // Symbols are NOT scanned here.  Every interned string is
                // perennial: SymbolTable::intern always builds the symbol
                // with a null ProtoContext, so its Cells are allocated via
                // posix_memalign directly — never enrolled in any thread
                // freelist or context young chain.  The GC's mark/sweep
                // machinery therefore never sees a symbol Cell as a
                // candidate, there is nothing to protect, and iterating
                // every SymbolTable shard on every GC cycle would be pure
                // overhead.  (There is no longer any "weak"/collectible
                // symbol variant.)

                // Phase 2 — capture the mutable-shard snapshot under STW.
                //
                // This is the formal "snapshot at the beginning" for the
                // concurrent mark that follows.  Workers are parked here
                // (Phase 1 quorum reached), so each per-shard load is
                // atomic by construction — no fence beyond `acquire` is
                // required.  Post-STW the workers will resume and may
                // CAS-swap shard roots freely; the marker never reads
                // the live table again, only this snapshot.
                //
                // The states are NOT roots (docs/GLOBAL_MUTABLE_TABLE.md,
                // "States are not roots").  A state is marked only when the
                // mark reaches its handle, through the handle's entry in this
                // snapshot (Marker::reportMutableState); the shard trees'
                // own nodes are marked before the trace, without their values
                // (indexSnapshot).  An unreachable chain or cycle of
                // mutable objects is therefore entirely unmarked and freed in
                // one cycle; its state cells are emptied and kept until no
                // scanned path reaches them (retireDeadStates).  The graph
                // reachable from the snapshot is immutable while the mark
                // runs, so the marker walks a frozen view.
                //
                // Without survivor re-inclusion a handle that survived a
                // cycle is never swept again, so its entry would never be
                // released and would keep naming a freed state for ever: in
                // that configuration the shard roots stay roots, as before.
                //
                // With more than one live space at the pause they stay roots
                // too.  A state is a cell of the space whose context wrote
                // it, and a handle of another space is reached only by that
                // space's roots: this space would free a state it allocated
                // for a handle it never reaches, while the handle lives
                // (docs/GLOBAL_MUTABLE_TABLE.md, "Several spaces").
                //
                // Cost: O(MUTABLE_ROOT_SHARDS) atomic reads, ~2 KB store.
                // Independent of heap size or live-object count.  No write
                // barriers anywhere in the runtime — the snapshot replaces
                // them.  See docs/STW_ELIMINATION_RESEARCH.md § 13.
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                const bool statesAreRoots = multispace::liveSpaceCount() > 1;
#else
                constexpr bool statesAreRoots = true;
#endif
                for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
                    ProtoSparseList* r =
                        globalMutableShards[s].root.load(std::memory_order_acquire);
                    space->gcMutableSnapshot[s] = r;
                    if (statesAreRoots && r) addRootObj(reinterpret_cast<const ProtoObject*>(r));
                }
                // From here until this cycle's Phase 5b, an entry whose handle
                // the sweep finds dead may name a freed state: whole-table
                // walkers that read values wait for it (findMutableCycles,
                // and the next cycle's token wait above).
                if (!statesAreRoots)
                    multispace::mutableReleasesPending.fetch_add(1, std::memory_order_seq_cst);
                if (space->threads) addRootObj(reinterpret_cast<const ProtoObject*>(space->threads));
                
                // Scan embedder-registered root sets.  Each set owns a
                // private collection of `ProtoObject*` pinned by a
                // runtime built on protoCore (e.g. protoJS callbacks
                // captured into C++ lambdas).  See ProtoRootSet for
                // the rationale; we are inside STW here, mutators are
                // parked, so no add/remove can race with iteration.
                //
                // The visit functions are non-capturing C function
                // pointers because the ProtoSpace API only exposes
                // that signature (kept C-friendly for FFIs).  We
                // route the addRootObj closure through a stack
                // context structure passed as `user`.
                struct RootSetVisitCtx {
                    std::vector<const Cell*>* rootList;
                };
                RootSetVisitCtx rsCtx{&rootList};
                space->forEachRootSet(
                    [](void* user, ProtoRootSet* rs) {
                        rs->forEachRoot(
                            [](void* u, const ProtoObject* obj) {
                                auto* c = static_cast<RootSetVisitCtx*>(u);
                                if (ProtoObject::isCellPointer(obj)) {
                                    c->rootList->push_back(
                                        ProtoObject::asCellPointer(obj));
                                }
                            },
                            user);
                    },
                    &rsCtx);

                // 3. Scan the Main Thread stack, unless a thread's chain
                // already covered it.
                if (!mainContextScanned) scanContexts(space->mainContext);

                // 6. Capture the heap snapshot (segments to process)
                // This MUST be done during STW to ensure we only sweep what existed at root collection.
                //
                // Survivor pen — staggered re-chain.  The previous sweep
                // pushed survivors into space->survivorPen instead of
                // dirtySegments.  Stop-the-world only captures the pen, in
                // O(1); the work on its segments runs after the world
                // resumes.  Two treatments, mutually exclusive within a
                // cycle:
                //
                //   FOLD CYCLE  (gcCycleCount % stagger == 0):
                //     Take the whole pen (exchange).  After the world
                //     resumes, its segments are linked in front of this
                //     cycle's segmentsToProcess, so their cells are
                //     candidates again.  Mark from roots reaches the live
                //     ones; outgoing references are traced via the normal
                //     candidate path.
                //
                //   NON-FOLD CYCLE (stagger > 1 only):
                //     Pen cells stay in the pen this cycle (skip mark and
                //     sweep cost).  But if a pen cell references a cell
                //     that IS a candidate, mark must still trace through
                //     it or the referenced cell is freed and the pen
                //     reference dangles.  Record the pen head; after the
                //     world resumes, mark walks each pen cell and pushes
                //     its outgoing references, as for the young chains.
                //     The pen cell itself is not pushed (not a candidate,
                //     no liveness check).
                //
                // With stagger == 1 (default) every cycle is a fold cycle.
                // With stagger > 1, only every Nth cycle folds; survivors
                // skip mark/sweep cost in the meantime, at the price of
                // delayed reclamation by up to N cycles.
                // Count every cycle, whatever the survivor configuration:
                // getGCCycleCount() and heap-limit reclamation waits read it.
                [[maybe_unused]] const uint64_t newCycle =
                    space->gcCycleCount.fetch_add(1, std::memory_order_relaxed) + 1;
                // The process-wide epoch that tells every thread, of any
                // space, to clear its caches at its next quiescent point.
                // seq_cst: lazy objects order their publication against it
                // (core/ProtoLazyObject.cpp).
                multispace::gcEpoch.fetch_add(1, std::memory_order_seq_cst);
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                const unsigned int stagger = space->survivorStagger ? space->survivorStagger : 1;
                const bool foldThisCycle = ((newCycle % stagger) == 0);
                // At most one of the two is non-null.
                DirtySegment* penToFold = nullptr;
                const DirtySegment* penToScan = nullptr;
                if (foldThisCycle) {
                    penToFold = space->survivorPen.exchange(nullptr, std::memory_order_acquire);
                } else {
                    penToScan = space->survivorPen.load(std::memory_order_acquire);
                }
#endif

                DirtySegment* segmentsToProcess = space->dirtySegments.exchange(nullptr, std::memory_order_acquire);

                // --- PHASE 3: RESUME THE WORLD ---
                //
                // The STW window closes HERE — before the mark phase, not
                // after it.  Mark now runs concurrent with user threads,
                // safely, because:
                //
                //   1. The mutable-shard snapshot captured in Phase 2
                //      (gcMutableSnapshot[]) gives the marker a frozen
                //      view of every reachable mutable.  Workers may CAS
                //      mutableRoot[s] freely; the marker never reads the
                //      live table.
                //
                //   2. Every Cell field traced by processReferences is
                //      const-qualified after construction.  Workers
                //      cannot mutate the fields the marker reads.
                //
                //   3. The mark bit on Cell::next_and_flags is touched
                //      ONLY by the GC thread.  Workers never call
                //      mark() / unmark() / isMarked().
                //
                //   4. segmentsToProcess was exchange()d atomically
                //      above; new dirty segments submitted by workers
                //      post-STW go to a fresh space->dirtySegments and
                //      survive to the next cycle.
                //
                //   5. New cells allocated by workers post-STW live in
                //      their per-context young chain, never in this
                //      cycle's segmentsToProcess — sweep does not see
                //      them.
                //
                // The change is described in detail in
                // docs/GarbageCollector.md § "Concurrent Mark Without
                // Barriers" and docs/STW_ELIMINATION_RESEARCH.md § 13.
                space->stwFlag.store(false);
                multispace::attention.fetch_sub(1);
                space->stopTheWorldCV.notify_all();
                const std::uint64_t stwNanos = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - stwStart).count());
                GC_LOCK_TRACE("gcLoop REL(mark)");
                lock.unlock(); // Mark, sweep, and bulk-unmark all run unlocked.
                // The cycle's always-on measures (adaptive::CycleMeasures):
                // three clock reads and a counter per swept cell.
                adaptive::CycleMeasures measures;
                measures.stwNanos = stwNanos;
                const auto markStart = std::chrono::steady_clock::now();
#ifdef PROTOCORE_GC_INSTRUMENT
                // P2 ends here, where the world resumes, so P1 + P2 is the
                // stop-the-world pause.  Everything below counts as mark (P4).
                auto t_phase4_start = std::chrono::steady_clock::now();
                dbg_ns_stw += stwNanos;
                if (stwNanos > dbg_ns_max_stw) dbg_ns_max_stw = stwNanos;
                dbg_roots += rootList.size();
                dbg_total_phase2_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase4_start - t_phase2_start).count(),
                    std::memory_order_relaxed);
#endif

                // Module roots recorded by Phase 2 are roots (P3).  Only this
                // space's own entries: the table is global, the tracing is not.
                // The conversion is addRootObj's own, from the Phase-2 root block
                // above: isCellPointer + asCellPointer, no hand-written cast.
                globalModuleRootTable().forEachCaptured(
                    space, &rootList, [](void* user, const ProtoObject* module) {
                        if (ProtoObject::isCellPointer(module))
                            static_cast<std::vector<const Cell*>*>(user)
                                ->push_back(ProtoObject::asCellPointer(module));
                    });

                // Interned tuples recorded by Phase 2 are roots.
                if (space->tupleInterner) {
                    space->tupleInterner->forEachCaptured(&rootList, [](void* user, const Cell* tuple) {
                        static_cast<std::vector<const Cell*>*>(user)->push_back(tuple);
                    });
                }

                // Young chains captured in Phase 2.  Their cells are neither
                // candidates of this cycle nor marked; the references they
                // hold are shaded so mark reaches every object a young cell
                // refers to (traceSources below).  The walk runs while the
                // mutators run, over a stable view.  Every chain was captured from a thread that
                // was parked or out of the running set at the capture: a
                // thread is in `space->threads` only while it counts in the
                // quorum (thread_main leaves both in one globalMutex section,
                // GitHub issue #3).  Then:
                //   * a chain grows only by prepending (addCell2Context sets
                //     the new cell's next to the current head, then publishes
                //     the new cell as head), so cells allocated after the
                //     capture sit in front of the captured head and no link
                //     behind it is rewritten;
                //   * submitting a chain (context destruction, safepoint
                //     threshold) hands its head to a DirtySegment without
                //     touching links; a segment pushed after the capture
                //     belongs to the next cycle, which cannot start before
                //     this mark ends;
                //   * the other writers of cell links are the freelist paths,
                //     which own free cells, and this cycle's sweep, which runs
                //     after mark on the segments captured in Phase 2 —
                //     disjoint from live young chains, because a submission
                //     detaches the chain from its context;
                //   * a reference field is written once, at construction, so
                //     a cell still being constructed at the capture is read
                //     with either its initial null or its final value.
#ifdef PROTOCORE_GC_INSTRUMENT
                t_young_start = std::chrono::steady_clock::now();
                c_young_start = dbgCpuNs();
#endif
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                // Survivor pen captured in Phase 2.
                //
                // Fold: the exchanged pen is private to this thread.  Linking
                // it in front of segmentsToProcess builds the list sweep
                // walks; the candidate set itself was fixed by the two
                // exchanges under stop-the-world.
                if (penToFold) {
                    DirtySegment* penTail = penToFold;
                    while (penTail->next) penTail = penTail->next;
                    penTail->next = segmentsToProcess;
                    segmentsToProcess = penToFold;
                }
                // Non-fold: the mark phase walks the pen captured in Phase 2
                // and shades the references of its cells (traceSources).
                // The view is stable: mutators never touch the pen or the links of its cells, this
                // cycle's sweep runs after mark and only prepends new
                // survivor segments at the pen head, and the pen is folded
                // only under a later cycle's stop-the-world.
#endif

                // --- PHASE 4: MARK (concurrent with mutators) ---
                //
                // Sweep only clears mark bits on cells inside
                // segmentsToProcess; cells outside that set (young
                // cells whose owning context never submitted, perpetual
                // prototypes, tuple/string interner entries) used to
                // retain stale mark=1 from prior cycles, which made
                // the next cycle's `if (!isMarked())` skip them and
                // their transitive closure — corrupting the live
                // graph (see commit 0441247e).
                //
                // The previous fix (pre-mark pass that walked the live
                // graph and unmarked every reachable cell, deduped via
                // std::set<const Cell*>) restored correctness but at
                // O(N log N) per cycle on the std::set inserts.  That
                // dominated CPU time on attribute-heavy workloads:
                // perf record on `binary_trees(10)` showed
                // _Rb_tree::_M_insert_unique alone at 15.8 % of total
                // CPU, with gcThreadLoop frame-time at 26 %, and the
                // bench dropped from 2.2 s/iter (2026-05-01) to ~12 s
                // /iter (2026-05-04).
                //
                // Replacement: track the cells we mark right here in
                // Phase 4 (one push_back per Cell, deduped naturally
                // by the `isMarked()` guard in Marker::shade), then
                // unmark them in a separate post-sweep pass.  Same
                // tricolour invariant restored, no std::set, O(N)
                // pure pushes — and the markedList is stored in
                // 256 KiB blocks, so traversing it post-sweep is
                // cache-friendly.
                //
                // Why post-sweep instead of pre-mark next cycle: it
                // localises the work to "the GC cycle that produced
                // the marks", so a single iteration of gcThreadLoop
                // is self-contained — no cross-iteration state
                // beyond the cell mark bits themselves.
                //
                // The trace (docs/GarbageCollector.md, "Mark work list").
                // Every source is traced as it is visited: each root is
                // shaded and each young or pen cell scanned, and the stack is
                // drained right after, so the stack holds one traversal at a
                // time instead of every source's references at once.  Each
                // context's young chain is walked once even if two routes
                // captured it.
                std::sort(youngChainHeads.begin(), youngChainHeads.end());
                youngChainHeads.erase(std::unique(youngChainHeads.begin(), youngChainHeads.end()),
                                      youngChainHeads.end());
                markedList.reset();
                markStack.beginCycle(gcmark::markStackCapacity());
                Marker marker{markStack, markedList, space->rootContext};
                // The table's nodes, and the index of its states (indexSnapshot).
                std::size_t tableNodesMarked = 0;
                if (!statesAreRoots) {
                    tableNodesMarked = indexSnapshot(marker, mutableStates, space->gcMutableSnapshot,
                                                     tableNodeStack);
                    marker.mutableStates = &mutableStates;
                }
                std::size_t youngCellsWalked = 0;
                auto traceSources = [&](bool firstPass) {
                    for (const Cell* root : rootList) {
                        marker.shade(root);
                        marker.drain();
                    }
                    for (const Cell* head : youngChainHeads) {
                        for (const Cell* scanCell = head; scanCell; scanCell = scanCell->getNext()) {
                            if (firstPass) {
                                ++youngCellsWalked;
#ifdef PROTOCORE_GC_INSTRUMENT
                                ++dbg_young_cells;
#endif
                            }
                            marker.scan(scanCell, "Phase4(young)");
                            marker.drain();
                        }
                    }
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                    for (const DirtySegment* penSeg = penToScan; penSeg; penSeg = penSeg->next) {
                        for (const Cell* scanCell = penSeg->cellChain; scanCell; scanCell = scanCell->getNext()) {
                            marker.scan(scanCell, "Phase4(pen)");
                            marker.drain();
                        }
                    }
#endif
                };
                traceSources(true);
#ifdef PROTOCORE_GC_INSTRUMENT
                t_trace_start = std::chrono::steady_clock::now();
                dbg_ns_young += dbgNs(t_young_start, t_trace_start);
#endif
                // Overflow: some shade found the stack full and left a cell
                // white.  That cell is referenced by a root, a young or pen
                // cell, or a marked cell, so tracing the sources again and
                // scanning every cell marked so far shades it.  Repeat until
                // a pass completes without overflow; then every reference of
                // every marked cell and source has been shaded with room on
                // the stack, and the marked set is closed.  Cells marked
                // during a pass were pushed and drained, so the scan only
                // needs the cells marked before it started.
                // The table's nodes come first in the marked list and are never
                // scanned: a scan would shade every state they hold.
                std::size_t overflowRescans = 0;
                auto closeTrace = [&]() {
                    while (marker.overflow) {
                        marker.overflow = false;
                        ++overflowRescans;
                        const std::size_t markedBefore = markedList.size();
                        traceSources(false);
                        for (std::size_t i = tableNodesMarked; i < markedBefore; ++i) {
                            marker.scan(markedList[i], "Phase4(rescan)");
                            marker.drain();
                        }
                    }
                };
                closeTrace();
                // The trace has closed: retire the states no handle reached
                // (retireDeadStates).
                if (!statesAreRoots) {
                    retireDeadStates(marker, mutableStates, spaceIdOf(space), retiredStates);
                    closeTrace();
                }
                {
                    gcmark::MarkStats ms;
                    ms.peakStackEntries = markStack.peak();
                    ms.stackCapacity = markStack.capacity();
                    ms.overflowRescans = overflowRescans;
                    ms.youngCellsWalked = youngCellsWalked;
                    ms.markedCells = markedList.size();
                    ms.markedListBytes = markedList.reservedBytes();
                    gcmark::recordMarkStats(ms);
#ifdef PROTOCORE_GC_INSTRUMENT
                    if (ms.peakStackEntries > dbg_mark_stack_peak) dbg_mark_stack_peak = ms.peakStackEntries;
                    if (ms.markedListBytes > dbg_marked_list_max_bytes) dbg_marked_list_max_bytes = ms.markedListBytes;
                    dbg_mark_rescans += ms.overflowRescans;
#endif
                }

                // Phase 3 (Resume The World) already happened above —
                // BEFORE the mark loop, not after.  Mark ran concurrent
                // with the mutators using gcMutableSnapshot.  Sweep
                // below continues to run unlocked, as it always did.
#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase5_start = std::chrono::steady_clock::now();
                dbg_ns_trace += dbgNs(t_trace_start, t_phase5_start);
                c_sweep_start = dbgCpuNs();
                dbg_cpu_mark += c_sweep_start - c_young_start;
                dbg_total_phase4_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase5_start - t_phase4_start).count(),
                    std::memory_order_relaxed);
                dbg_total_cells_marked.fetch_add(
                    markedList.size(), std::memory_order_relaxed);
                {
                    DirtySegment* s = segmentsToProcess;
                    uint64_t segCount = 0;
                    while (s) { segCount++; s = s->next; }
                    dbg_total_segments_swept.fetch_add(segCount, std::memory_order_relaxed);
                }
#endif

                // --- PHASE 5: SWEEP ---
                //
                // Cross-segment chunk accumulator (path #5 v2).  Dead cells
                // from each segment are chained into a running chunk; when
                // the chunk reaches CELL_CHUNK_SIZE, it is published to
                // `space->freeChunks` and a fresh chunk starts.  At the end
                // of sweep the partial trailing chunk is published with its
                // actual count.  This converts getFreeCells from an O(N)
                // walk-and-cut into an O(1) chunk pop.
                const auto sweepStart = std::chrono::steady_clock::now();
                measures.markNanos = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(sweepStart - markStart).count());
                // With other spaces live, a thread of another space may still
                // hold, in C++ locals, a cell this sweep finds dead: a table
                // node or state read before this cycle's stop-the-world, which
                // did not stop it.  The dead cells are then only collected
                // here and freed after a grace period (see below and
                // docs/GLOBAL_MUTABLE_TABLE.md); sweep must not even rewrite
                // their header.  With one space they are freed in place.
                const bool deferFree = multispace::liveSpaceCount() > 1;
                std::vector<Cell*> deadCells;

                // The collector thread sweeps; while mutators wait for
                // headroom, the helpers of the process-wide pool claim runs
                // of segments too (core/Sweep.h).  On return everything is
                // published and the helpers' finalized refs are merged.
                //
                // A space being destroyed abandons the rest of its sweep.
                // ~ProtoSpace raises `ending` and joins this thread; nothing
                // will allocate from the cells the sweep would free, and
                // protoCore runs no finalizer at exit (docs/MemoryModel.md,
                // "Two further limits"), so a cell left unswept is treated as
                // every cell still live at exit is.  The sweepers stop claiming
                // segments once the flag is up (sweep::sweepCycle).  What the
                // cycle still does is what another space depends on: clear the
                // mark bits it set (Phase 6; perennial and interned cells are
                // shared by the process), clear its snapshot, settle
                // mutableReleasesPending and hand on the cycle token.  The
                // space's entries in the global mutable table, those of the
                // handles already finalized included, are purged by the next
                // cycle of a live space, as when the space is destroyed with
                // its collector idle (recordDestroyedSpace).
                if (auto hook = sweep::beforeSweepForTest())
                    hook(space);
                const sweep::CycleSweep sweeper =
                    sweep::sweepCycle(space, segmentsToProcess, deferFree, deadCells,
                                      &space->impl->ending);
                // Total cells reclaimed this cycle, across all published
                // chunks — the out-of-memory signal (see reclaimedLastCycle).
                proto_ulong reclaimedThisCycle = sweeper.reclaimed;
#ifdef PROTOCORE_GC_INSTRUMENT
                dbg_swept_cells += sweeper.swept;
#endif
                measures.sweepNanos = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - sweepStart).count());
                measures.sweptCells = sweeper.swept;
                measures.sweptSegments = sweeper.segments;
                measures.wideSegments = sweeper.wideSegments;
                measures.narrowSegments = sweeper.narrowSegments;

#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_release_start = std::chrono::steady_clock::now();
                dbg_cpu_sweep += dbgCpuNs() - c_sweep_start;
                dbg_total_phase5_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_release_start - t_phase5_start).count(),
                    std::memory_order_relaxed);
#endif

                // --- PHASE 5b: RELEASE MUTABLES-TREE ENTRIES ---
                //
                // Sweep finalized the handles of unreachable mutable objects;
                // their finalizers recorded the mutable_refs.  Remove those
                // entries from mutableRoot now, one compare-and-swap per shard,
                // allocating through the collector's own context.  The
                // states they held were not marked through their handles and
                // were freed by this sweep (states are not roots).  See
                // releaseFinalizedMutableEntries.
                if (!statesAreRoots) emptyRetiredStates(retiredStates);
                if (!deferFree) {
                    if (!sweeper.abandoned) releaseFinalizedMutableEntries(space);
                    if (!statesAreRoots)
                        multispace::mutableReleasesPending.fetch_sub(1, std::memory_order_seq_cst);
                }

#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase6_start = std::chrono::steady_clock::now();
                dbg_total_release_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase6_start - t_release_start).count(),
                    std::memory_order_relaxed);
#endif

                // --- PHASE 6: BULK UNMARK (replaces the pre-mark pass) ---
                //
                // Walk the markedList collected in Phase 4 and unmark
                // every entry.  After this completes, ALL reachable
                // cells (segments-side and outside-segments alike)
                // satisfy mark=0, so the next cycle's mark phase sees
                // a clean tricolour state.
                //
                // Sweep already cleared the mark bit on the survivors
                // it kept (the `else { cell->unmark(); }` branch in
                // Phase 5).  For those entries this loop's
                // fetch_and(~0x1) is a benign no-op — atomic and
                // cheap.  The interesting work is on the cells that
                // were marked but live OUTSIDE segmentsToProcess
                // (perpetuals, unsubmitted-young, interners) — sweep
                // never touched their mark bit, and without this loop
                // they would carry mark=1 into the next cycle and
                // re-trigger the bug 0441247e fixed.
                //
                // Runs OUTSIDE the STW window (lock was released
                // before Phase 5) — same locking regime as sweep.
                // Mutators may be allocating fresh cells in parallel;
                // those new cells are not in markedList and start
                // with mark=0, so they are unaffected.  unmark() is
                // a single atomic fetch_and, safe regardless of
                // contention with mutator threads.
                // Test before writing: a survivor the sweep already
                // unmarked is only read, so the mutators' shared copies of
                // its line stay valid; a cell marked outside the segments
                // (perennial, young, interned) is still cleared.
                // Each entry costs a miss on the cell's header; prefetching a
                // few entries ahead overlaps them.
                {
                    constexpr std::size_t kAhead = 16;
                    const std::size_t n = markedList.size();
                    for (std::size_t i = 0; i < n; ++i) {
                        if (i + kAhead < n) {
                            const Cell* ahead = markedList[i + kAhead];
                            if ((reinterpret_cast<uintptr_t>(ahead) & 0x3F) == 0) PROTO_PREFETCH(ahead);
                        }
                        const Cell* m = markedList[i];
                        if (m && (reinterpret_cast<uintptr_t>(m) & 0x3F) == 0 && m->isMarked()) {
                            const_cast<Cell*>(m)->unmark();
                        }
                    }
                }
#ifdef PROTOCORE_GC_INSTRUMENT
                dbg_ns_unmark += dbgNs(t_phase6_start, std::chrono::steady_clock::now());
#endif

                // --- PHASE 7: CLEAR MUTABLE SNAPSHOT ---
                //
                // The snapshot was the formal mark-time dereference
                // table for THIS cycle only.  Clear all entries so the
                // next cycle's Phase 2 starts from a known nullptr
                // baseline, and so any stray GC-side read of the table
                // outside a cycle observes nullptr instead of a stale
                // shard root.
                //
                // Safe to run unlocked (same regime as the bulk-unmark
                // loop above): only the GC thread reads or writes the
                // snapshot table.
                for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
                    space->gcMutableSnapshot[s] = nullptr;
                }
                // The mark bits this cycle set are all cleared (Phase 6):
                // hand the token to the next cycle of any space.
                multispace::noteCycleEnd();
                {
                    std::lock_guard<std::recursive_mutex> token(ProtoSpace::globalMutex);
                    multispace::cycleActive = false;
                }
                multispace::cycleCV().notify_all();

                if (deferFree && sweeper.abandoned) {
                    // The space is going away: its dead cells are not
                    // finalized (no grace period is waited for them either).
                    if (!statesAreRoots) {
                        multispace::mutableReleasesPending.fetch_sub(1, std::memory_order_seq_cst);
                        { std::lock_guard<std::recursive_mutex> wake(ProtoSpace::globalMutex); }
                        multispace::cycleCV().notify_all();
                    }
                } else if (deferFree) {
                    // Grace period: every registered thread of the process
                    // passes a safepoint outside any critical section, or is
                    // parked or out of its quorum, before a dead cell of this
                    // cycle is finalized or reused.  At such a point a thread
                    // holds no cell reachable only from C++ locals -- the
                    // stop-the-world's own invariant -- and it clears its
                    // caches there.  No thread is stopped.
                    multispace::waitForGracePeriod();
                    Cell* head = nullptr;
                    Cell* tail = nullptr;
                    proto_ulong count = 0;
                    for (Cell* dead : deadCells) {
                        dead->finalize(space->rootContext);
                        dead->internalSetNextRaw(head);
                        if (!tail) tail = dead;
                        head = dead;
                        if (++count >= ProtoSpace::CELL_CHUNK_SIZE) {
                            std::lock_guard<std::recursive_mutex> chunkLock(ProtoSpace::globalMutex);
                            publishFreeChunk(space, head, tail, count);
                            head = tail = nullptr;
                            count = 0;
                        }
                    }
                    if (head) {
                        std::lock_guard<std::recursive_mutex> chunkLock(ProtoSpace::globalMutex);
                        publishFreeChunk(space, head, tail, count);
                    }
                    reclaimedThisCycle += deadCells.size();
                    // Phase 5b, after the finalizers recorded the refs.  It
                    // reads the live table, so this thread is not quiescent
                    // meanwhile: another space's grace period waits for it.
                    multispace::setQuiescenceOut(false);
                    releaseFinalizedMutableEntries(space);
                    if (!statesAreRoots) {
                        multispace::mutableReleasesPending.fetch_sub(1, std::memory_order_seq_cst);
                        // A cycle may wait for it at the token (see there).
                        { std::lock_guard<std::recursive_mutex> wake(ProtoSpace::globalMutex); }
                        multispace::cycleCV().notify_all();
                    }
                    multispace::setQuiescenceOut(true);
                }
#ifdef PROTOCORE_GC_INSTRUMENT
                auto t_phase6_end = std::chrono::steady_clock::now();
                dbg_total_phase6_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        t_phase6_end - t_phase6_start).count(),
                    std::memory_order_relaxed);
                if (dbg_profile) {
                    uint64_t cycles = space->gcCycleCount.load(std::memory_order_relaxed);
                    // 2026-05-23 night: print every cycle (was every 5) to
                    // catch short benchmarks. Revert before merging if a
                    // long-running test floods stderr.
                    if (cycles > 0) {
                        std::fprintf(stderr,
                            "[GC-PROFILE] cycles=%" PROTO_FMT_U "  P1=%" PROTO_FMT_U "us  P2=%" PROTO_FMT_U "us  P4=%" PROTO_FMT_U "us  P5=%" PROTO_FMT_U "us  REL=%" PROTO_FMT_U "us  P6=%" PROTO_FMT_U "us  marked=%" PROTO_FMT_U "  swept_segs=%" PROTO_FMT_U "\n",
                            (proto_ulong)cycles,
                            (proto_ulong)dbg_total_phase1_us.load(),
                            (proto_ulong)dbg_total_phase2_us.load(),
                            (proto_ulong)dbg_total_phase4_us.load(),
                            (proto_ulong)dbg_total_phase5_us.load(),
                            (proto_ulong)dbg_total_release_us.load(),
                            (proto_ulong)dbg_total_phase6_us.load(),
                            (proto_ulong)dbg_total_cells_marked.load(),
                            (proto_ulong)dbg_total_segments_swept.load());
                    }
                }
                dbg_freed_cells += reclaimedThisCycle;
                dbg_ns_busy += dbgNs(t_token_end, t_phase6_end);
                dbg_cpu_busy += dbgCpuNs() - c_token_end;
                if (dbg_profile) {
                    // One line per cycle, cumulative, nanosecond totals in
                    // microseconds.  mut_park/headroom are summed over all
                    // mutator threads.
                    std::fprintf(stderr,
                        "[GC-PHASES] cycles=%" PROTO_FMT_U " busy=%" PROTO_FMT_U "us token=%" PROTO_FMT_U "us quorum=%" PROTO_FMT_U "us stw=%" PROTO_FMT_U "us stw_max=%" PROTO_FMT_U "us roots=%" PROTO_FMT_U
                        " young=%" PROTO_FMT_U "us young_cells=%" PROTO_FMT_U " trace=%" PROTO_FMT_U "us marked=%" PROTO_FMT_U
                        " sweep=%" PROTO_FMT_U "us swept_cells=%" PROTO_FMT_U " freed_cells=%" PROTO_FMT_U " rel=%" PROTO_FMT_U "us unmark=%" PROTO_FMT_U "us p6_7=%" PROTO_FMT_U "us"
                        " mut_park=%" PROTO_FMT_U "us parks=%" PROTO_FMT_U " headroom_wait=%" PROTO_FMT_U "us headroom_waits=%" PROTO_FMT_U
                        " cpu_busy=%" PROTO_FMT_U "us cpu_mark=%" PROTO_FMT_U "us cpu_sweep=%" PROTO_FMT_U "us"
                        " refill_fresh=%" PROTO_FMT_U " refill_recycled=%" PROTO_FMT_U " helper_runs=%" PROTO_FMT_U
                        " wide_segments=%" PROTO_FMT_U " narrow_segments=%" PROTO_FMT_U
                        " mark_stack_peak=%" PROTO_FMT_U " mark_rescans=%" PROTO_FMT_U " marked_list_max_kb=%" PROTO_FMT_U "\n",
                        (proto_ulong)space->gcCycleCount.load(std::memory_order_relaxed),
                        (proto_ulong)(dbg_ns_busy / 1000), (proto_ulong)(dbg_ns_token / 1000),
                        (proto_ulong)(dbg_ns_quorum / 1000), (proto_ulong)(dbg_ns_stw / 1000),
                        (proto_ulong)(dbg_ns_max_stw / 1000), (proto_ulong)dbg_roots,
                        (proto_ulong)(dbg_ns_young / 1000), (proto_ulong)dbg_young_cells,
                        (proto_ulong)(dbg_ns_trace / 1000), (proto_ulong)dbg_total_cells_marked.load(),
                        (proto_ulong)dbg_total_phase5_us.load(), (proto_ulong)dbg_swept_cells,
                        (proto_ulong)dbg_freed_cells, (proto_ulong)dbg_total_release_us.load(),
                        (proto_ulong)(dbg_ns_unmark / 1000), (proto_ulong)dbg_total_phase6_us.load(),
                        (proto_ulong)(gcprof::mutatorParkNs.load(std::memory_order_relaxed) / 1000),
                        (proto_ulong)gcprof::mutatorParks.load(std::memory_order_relaxed),
                        (proto_ulong)(gcprof::headroomWaitNs.load(std::memory_order_relaxed) / 1000),
                        (proto_ulong)gcprof::headroomWaits.load(std::memory_order_relaxed),
                        (proto_ulong)(dbg_cpu_busy / 1000), (proto_ulong)(dbg_cpu_mark / 1000),
                        (proto_ulong)(dbg_cpu_sweep / 1000),
                        (proto_ulong)gcprof::refillFreshCells.load(std::memory_order_relaxed),
                        (proto_ulong)gcprof::refillRecycledCells.load(std::memory_order_relaxed),
                        (proto_ulong)sweep::poolStats().helperRuns,
                        (proto_ulong)sweep::walkStats().wideSegments,
                        (proto_ulong)sweep::walkStats().narrowSegments,
                        (proto_ulong)dbg_mark_stack_peak, (proto_ulong)dbg_mark_rescans,
                        (proto_ulong)(dbg_marked_list_max_bytes / 1024));
                }
#endif

                GC_LOCK_TRACE("gcLoop ACQ(after-sweep)");
                lock.lock(); // Re-acquire for next wait
                space->gcStarted = false;
                // Publish the cycle's accounting for the heap-limit path:
                //  * reclaimedLastCycle — cells the sweep returned to the
                //    freelist; the authoritative out-of-memory signal
                //    (waitForHeapHeadroom: two zero-reclaim cycles == OOM).
                //  * liveCellsLastCycle — mark-phase reachable count, used
                //    only for the OOM abort's diagnostic message.
                // Then wake any thread parked waiting for the sweep to refill
                // the freelist (allocation-limit path).
                space->reclaimedLastCycle.store(reclaimedThisCycle,
                                                std::memory_order_relaxed);
                space->liveCellsLastCycle.store(markedList.size(),
                                                std::memory_order_relaxed);
                // The adaptive heap controller, when enabled, sets the next
                // soft limit from this cycle's live set and pressure.
                measures.freedCells = reclaimedThisCycle;
                adaptive::onCycleEnd(space, measures);
                space->memoryReclaimedCV.notify_all();
                space->gcCV.notify_all();
            }
        }
    }
    
    std::recursive_mutex ProtoSpace::globalMutex;

    ProtoSpace::ProtoSpace() :
        state(SPACE_STATE_RUNNING),
        gcThread(nullptr),
        booleanPrototype(nullptr),
        unicodeCharPrototype(nullptr),
        listPrototype(nullptr),
        sparseListPrototype(nullptr),
        tuplePrototype(nullptr),
        stringPrototype(nullptr),
        setPrototype(nullptr),
        multisetPrototype(nullptr),
        rangeIteratorPrototype(nullptr),
        attributeNotFoundGetCallback(nullptr),
        nonMethodCallback(nullptr),
        parameterTwiceAssignedCallback(nullptr),
        parameterNotFoundCallback(nullptr),
        runningThreads(1), // Main thread starts running
        stwFlag(false),
        parkedThreads(0),
        blocksPerAllocation(8192),  // Larger default batch to reduce getFreeCells calls during script load (was 1024; 1151 calls observed for multithread benchmark)
        heapSize(0),
        maxHeapSize(0),
        softHeapLimit(0),
        freeCellsCount(0),
        gcSleepMilliseconds(10),
        tupleRoot(nullptr),
        stringInternMap(nullptr),
        dirtySegments(nullptr),
        dirtySegmentFreePool(nullptr),
        survivorPen(nullptr),
        survivorStagger(SURVIVOR_STAGGER_DEFAULT),
        gcCycleCount(0),
        liveCellsLastCycle(0),
        reclaimedLastCycle(0),
        freeCells(nullptr),
        freeCellsTail(nullptr),
        freeChunks(nullptr),
        freeChunkPool(nullptr),
        gcStarted(false),
        mainContext(nullptr),
        nextMutableRef(1),
        resolutionChain_(nullptr),
        rootContext(nullptr)
    {
        // Record the OS thread that created this space.  ProtoContext uses this
        // to auto-inherit the main thread's ProtoThreadImplementation when a
        // context is created with previous == nullptr (i.e. every runtime's
        // bootstrap context), enabling per-thread attribute and mutable-value
        // caches without requiring each runtime to pass rootContext explicitly.
        this->mainThreadId = std::this_thread::get_id();

        // A process-unique id, carried in the high bits of every mutable_ref
        // this space creates, so a ref names its space and no two spaces
        // share one (docs/GLOBAL_MUTABLE_TABLE.md).  Registered before the
        // first mutable object of the bootstrap below.
        {
            const proto_ulong id = multispace::registerSpace(this);
            this->nextMutableRef.store((id << kMutableRefSpaceShift) | PROTO_UL(1));
        }

        // The tuple interner must exist before the first tuple is built.
        this->tupleInterner = new TupleInterner();

        // Per-context GC threshold: env var override, fall back to default.
        // Only consumed when PROTOCORE_GC_REINCLUDE_SURVIVORS is enabled, but
        // initialised unconditionally so the field is well-defined.
        this->maxAllocatedCellsPerContext = CONTEXT_GC_THRESHOLD_DEFAULT;
        if (const char* envThreshold = std::getenv("PROTOCORE_GC_CONTEXT_THRESHOLD")) {
            char* endPtr = nullptr;
            proto_ulong parsed = std::strtoul(envThreshold, &endPtr, 10);
            if (endPtr && *endPtr == '\0' && parsed > 0 && parsed <= UINT_MAX) {
                this->maxAllocatedCellsPerContext = static_cast<unsigned int>(parsed);
            }
        }

        // Survivor-pen stagger: how many GC cycles between successive
        // folds of the survivor pen back into dirtySegments.  Default 1
        // (re-check every cycle, no stagger).  Higher values reduce mark
        // cost on workloads with stable working sets at the price of
        // delayed reclamation (RSS may grow up to N × working set).
        this->survivorStagger = SURVIVOR_STAGGER_DEFAULT;
        if (const char* envStagger = std::getenv("PROTOCORE_GC_SURVIVOR_STAGGER")) {
            char* endPtr = nullptr;
            proto_ulong parsed = std::strtoul(envStagger, &endPtr, 10);
            if (endPtr && *endPtr == '\0' && parsed >= 1 && parsed <= 256) {
                this->survivorStagger = static_cast<unsigned int>(parsed);
            }
        }

        // Pre-allocate a pool of DirtySegments so submitYoungGeneration's
        // hot path (per-context threshold submission, every context-
        // destruction) can claim one without falling back to a system
        // malloc.  Each DirtySegment is 16 bytes (two pointers), so the
        // initial reservation is small and amortises the cost of every
        // future submission against this single startup burst.
        //
        // Profile of protoPython str_concat_loop with the threshold
        // submission active showed _int_malloc inside libc as a top
        // hotspot — every miss in the pool walked
        // posix_memalign / new — when the burst rate exceeded the GC's
        // sweep cadence (sweep is the only path that returns segments).
        // The pre-allocation keeps the pool warm so steady-state
        // submission is allocation-free, and the existing fallback in
        // submitYoungGeneration still covers pathological pressure.
        constexpr int kInitialDirtySegmentPool = 128;
        for (int i = 0; i < kInitialDirtySegmentPool; ++i) {
            DirtySegment* seg = new DirtySegment();
            seg->cellChain = nullptr;
            seg->next = this->dirtySegmentFreePool.load(std::memory_order_relaxed);
            while (!this->dirtySegmentFreePool.compare_exchange_weak(
                    seg->next, seg,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                // seg->next reloaded by compare_exchange_weak on failure.
            }
        }

        // Initialize prototypes
        this->rootContext = new ProtoContext(this, nullptr, nullptr, nullptr, nullptr, nullptr);
        this->booleanPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->unicodeCharPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->listPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->sparseListPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->mapPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->mpscQueuePrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        {
            // State added after the 3.2.0 public layout (ProtoLinkedList spec, D7).
            // Its objects are rooted by a root set of their own, which the
            // collector already scans: nothing is added to the collector's
            // own root scan.
            auto* extension = new ProtoSpaceImplementation();
            extension->roots = this->createRootSet("protoCore-space-extension");
            extension->linkedListPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
            extension->linkedListPrototypeHandle = extension->roots->add(extension->linkedListPrototype);
            for (auto*& shard : extension->walkPins)
                shard = this->createRootSet("protoCore-walk-pins");
            extension->mainThreadWalkPins = createSingleWriterRootSet(this, "protoCore-main-thread-walk-pins");
            // Owned by the constructing thread, which is the space's main
            // thread; see tlMainWalkPinSpace.
            tlMainWalkPinSpace = this;
            tlMainWalkPinSpaceId = spaceIdOf(this);
            this->impl = extension;
        }
        // Mutable so embedders (protoJS Object.prototype, protoPython
        // object.__class__, etc.) can install methods and accept user-
        // level setattr without forking the identity on every write.
        // The cell still serves as the root of the prototype chain; it
        // is allocated through the root context so the GC tracks it.
        this->objectPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(true));
        this->tuplePrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->stringPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->setPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->multisetPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->rangeIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false));
        this->threads = reinterpret_cast<ProtoSparseList*>(const_cast<ProtoObject*>(this->rootContext->newSparseList()->asObject(this->rootContext)));

        // Initialize all other prototypes to a basic object for now
        // This prevents null dereferences if getPrototype is called on an uninitialized type
        this->smallIntegerPrototype = this->objectPrototype;
        this->largeIntegerPrototype = this->objectPrototype;
        this->floatPrototype = this->objectPrototype; // Assuming float is not yet implemented as distinct from double
        this->bytePrototype = this->objectPrototype;
        this->nonePrototype = this->objectPrototype; // PROTO_NONE is a special constant, but its prototype can be objectPrototype
        this->methodPrototype = this->objectPrototype;
        this->bufferPrototype = this->objectPrototype;
        this->pointerPrototype = this->objectPrototype;
        this->doublePrototype = this->objectPrototype;
        this->datePrototype = this->objectPrototype;
        this->timestampPrototype = this->objectPrototype;
        this->timedeltaPrototype = this->objectPrototype;
        this->threadPrototype = this->objectPrototype;
        this->rootObject = this->objectPrototype; // Root object can be the base object prototype

        this->listIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->tupleIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->stringIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->sparseListIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->setIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));
        this->multisetIteratorPrototype = const_cast<ProtoObject*>(this->rootContext->newObject(false)->addParent(this->rootContext, this->objectPrototype));

        // The mutable table is process-global (docs/GLOBAL_MUTABLE_TABLE.md).
        // The first space installs an empty, perennial sparse list in every
        // shard; later spaces find them in place.  `mutableRoot` stays in the
        // class only to keep the ABI 3 layout and is never read.  Also zero
        // the per-cycle GC snapshot of the shard table; the snapshot is
        // populated under STW at every Phase 2 and cleared at the end of every
        // cycle, so steady-state it is always nullptr outside the cycle.
        {
            std::lock_guard<std::recursive_mutex> lock(globalMutex);
            // The side state of pacing and of the headroom waits, for every
            // space (adaptive::registerSpace; removed by forgetSpace).
            adaptive::registerSpace(this);
            for (int s = 0; s < MUTABLE_ROOT_SHARDS; ++s) {
                this->mutableRoot[s].root.store(nullptr);
                this->gcMutableSnapshot[s] = nullptr;
                if (globalMutableShards[s].root.load() == nullptr) {
                    auto* emptyRaw = new(static_cast<ProtoContext*>(nullptr))
                        ProtoSparseListImplementation(nullptr, 0, PROTO_NONE, nullptr, nullptr, true);
                    initialMutableShardRoots[s] = emptyRaw->asSparseList(nullptr);
                    globalMutableShards[s].root.store(
                        const_cast<ProtoSparseList*>(initialMutableShardRoots[s]));
                }
            }
        }

        // P3: interning is process-global.  This is a BORROWED pointer to the one
        // table of this process; the destructor must not free it.  Keeping the
        // field (rather than calling globalSymbolTable() at each use) leaves all
        // eleven existing `ctx->space->symbolTable` call sites untouched, keeps
        // the ProtoSpace layout unchanged, and preserves the mid-construction
        // sentinel that the six null checks in core/ProtoObject.cpp rely on: the
        // field is null before this line runs and non-null after, per space,
        // regardless of what other spaces have done.
        symbolTable = &globalSymbolTable();
        initStringInternMap(this);
        this->literalData         = const_cast<ProtoString*>(ProtoString::createSymbol(this->rootContext, "__data__"));
        this->literalSetAttribute = const_cast<ProtoString*>(ProtoString::createSymbol(this->rootContext, "setAttribute"));
        this->literalCallMethod   = const_cast<ProtoString*>(ProtoString::createSymbol(this->rootContext, "callMethod"));

        this->resolutionChain_ = buildDefaultResolutionChain(this->rootContext);

        // Adopt the OS process's main thread so the per-thread caches
        // (attribute, mutable-value, free-cells) are reachable through
        // `context->thread` on every getAttribute / setAttribute call
        // from the main thread.  Without this the main thread silently
        // ran with cache=nullptr, which translated to 0 hits and made
        // every attribute read fall through to the prototype-chain
        // walk + AVL implGetAt traversal — observed as 113.6× geomean
        // on pyperformance and 14.5% getAttribute / 11.2% implGetAt
        // in perf profiles of richards_lite.
        new (this->rootContext)
            ProtoThreadImplementation(ProtoThreadImplementation::AdoptMainThreadTag{},
                                       this->rootContext, /*name=*/nullptr, this);

        // The collector's own allocation context.  Built before the GC
        // thread starts, which is the only thread that allocates through it.
        this->gcContext = new ProtoContext(ProtoContext::GCOwnedTag{}, this);

        this->gcThread = std::make_unique<std::thread>(gcThreadLoop, this);

        // Heap limit from the environment:
        //   PROTOCORE_HEAP_LIMIT_CELLS=<hard>          hard ceiling only
        //   PROTOCORE_HEAP_LIMIT_CELLS=<soft>,<hard>   soft watermark and ceiling
        // in cells.  Without a limit protoCore starts no collection cycle by
        // itself, so this is how an embedder runs under a low memory limit
        // (in tests, for example) without calling setHeapLimits.
        //
        // A single value sets only the hard ceiling (soft 0): a soft
        // watermark equal to the ceiling is never consulted, because
        // getFreeCells enters the soft path only while the heap is below the
        // ceiling, so soft 0 states the actual behaviour.  With the ceiling
        // alone the heap grows up to <hard> cells and, from then on, a thread
        // that needs cells waits for a cycle to reclaim them.
        //
        // Applied last, once the space is fully built, so the bootstrap
        // above never waits on a limit.  Each part must be decimal digits
        // with a value up to INT_MAX; a hard part of 0 means no limit.  Any
        // other value is ignored silently, like the other PROTOCORE_*
        // variables read here.
        {
            int softCells = 0;
            int hardCells = 0;
            if (adaptive::parseHeapLimitCells(std::getenv("PROTOCORE_HEAP_LIMIT_CELLS"),
                                              softCells, hardCells)
                && hardCells > 0) {
                this->setHeapLimits(softCells, hardCells);
            }
        }
        // PROTOCORE_ADAPTIVE_HEAP=1, a diagnostic switch: every space runs the
        // adaptive heap controller from its creation, with the default
        // configuration (PROTOCORE_HEAP_LIMIT_CELLS, when set, gives H and
        // S0), and setHeapLimits leaves it enabled.  It measures an existing
        // embedder under the controller without rebuilding it.
        if (adaptive::environmentMode() == 1) {
            std::lock_guard<std::recursive_mutex> lock(globalMutex);
            adaptive::enable(this, AdaptiveHeapConfig());
        }
        // The pins of in-flight ProtoExceptions (core/ProtoException.cpp).
        this->exceptionRoots_ = this->createRootSet("protocore-exceptions-in-flight");
    }

    ProtoSpace::~ProtoSpace() {
        {
            std::lock_guard<std::recursive_mutex> lock(globalMutex);
            this->state = SPACE_STATE_ENDING;
            // The sweep in progress, if any, stops claiming segments.
            if (impl) impl->ending.store(true, std::memory_order_relaxed);
            // No thread of another space parks for this space, or rejoins its
            // quorum, from here on.
            multispace::unregisterSpace(this);
            // Its entries in the global mutable table are removed by the next
            // cycle of a live space.
            multispace::recordDestroyedSpace(spaceIdOf(this));
            this->gcCV.notify_all();
            // Its collector may be waiting for the cycle token.
            multispace::cycleCV().notify_all();
            this->stopTheWorldCV.notify_all();
        }
        if (gcThread && gcThread->joinable()) {
            // While it joins, this thread passes no quiescent point.  With
            // several spaces live, the collector being joined may be inside a
            // cycle whose grace period waits for every registered thread of
            // the process, this one included: mark it out of grace periods,
            // as a parked thread is, for the join.  It holds no cell of any
            // space across the join.
            multispace::setQuiescenceOut(true);
            gcThread->join();
            // The last space stops the sweep's helper threads; a later space
            // starts a new pool.  No collector of this space exists any more.
            sweep::shutdownPoolIfNoSpaces();
            sweep::forgetSpace(this);
            multispace::setQuiescenceOut(false);
        }
        {
            // Out of the process heap total and the controller registry.
            std::lock_guard<std::recursive_mutex> lock(globalMutex);
            adaptive::forgetSpace(this);
        }

        // Threads still running at destruction.  The contract
        // (docs/EMBEDDER-CONFORMANCE.md) is to join a space's ProtoThreads
        // before destroying it; a runtime that cannot -- a daemon thread at
        // process exit -- gets this instead of a use-after-free.  Every
        // other thread counted in this space's quorum is stopped as for a
        // collection, and the stop is never lifted:
        //   - a thread that parks (at an allocation, a safepoint, a heap
        //     wait) leaves the park for multispace::terminalPark and blocks
        //     there for the rest of the process;
        //   - a thread out in an unmanaged region (blocking I/O, sleepFor)
        //     counts as stopped now and blocks the same way when it returns,
        //     before it touches anything of the space.
        // Neither ever runs code of this space again, and nothing of theirs
        // is torn down: their stacks and thread-local storage stay as they
        // are, and joining one of them never returns.  The destructor waits
        // for every such thread to reach a safepoint or leave managed code,
        // exactly as a collection does; a thread that does neither (a native
        // loop with no safepoint) keeps it waiting.  The collector has been
        // joined above, so the stop-the-world flag is this stop's alone.
        {
            std::unique_lock<std::recursive_mutex> lock(globalMutex);
            const int self = (multispace::isMember(this) && !multispace::isOutOf(this)) ? 1 : 0;
            if (impl && runningThreads.load() > self) {
                stwFlag.store(true);
                multispace::attention.fetch_add(1);
                gcCV.wait(lock, [this, self] {
                    return parkedThreads.load() + self >= runningThreads.load();
                });
                impl->terminal.store(true, std::memory_order_seq_cst);
                stopTheWorldCV.notify_all();
                gcCV.wait(lock, [this] {
                    return impl->parkWaiters.load(std::memory_order_seq_cst) == 0;
                });
                multispace::attention.fetch_sub(1);
            }
        }

        // PROTOCORE_MUTABLE_CYCLE_CHECK -- the zero-code way to ask rule 13's
        // question of a whole program.
        //
        // Why here and nowhere else.  A cycle among mutable objects is never
        // collected (docs/MemoryModel.md section 7), and the table at teardown is
        // the most informative view there is: every entry still in it either
        // belongs to something still live or is in a cycle.  The GC thread has
        // joined, so nothing can free a cell under the walk and no critical
        // section is needed; `rootContext` is still alive, so attribute names
        // still render; and it costs exactly one `getenv` when the variable is
        // unset.
        //
        // Deliberately NOT hooked into the end of a GC cycle.  The walk is
        // O(live mutable graph), and a per-cycle hook would need a frequency
        // policy, would stall the collector on a schedule nobody chose, and would
        // have to be tuned per runtime.  A long-running process that never exits
        // should call findMutableCycles itself at a quiescent point -- it is three
        // lines, which is why it is public.
        //
        // The value selects the destination.  "1" (or "stderr") writes to
        // stderr; ANYTHING ELSE is a file path the report is APPENDED to, one
        // block per space, with the process id.  The file form is not a
        // convenience: a test suite that diffs a script's stderr fails the moment
        // a diagnostic appears there, which is exactly the suite a maintainer
        // wants to sweep with this, so writing to stderr would make the most
        // valuable use of the variable impossible.
        if (const char* dest = std::getenv("PROTOCORE_MUTABLE_CYCLE_CHECK")) {
            const MutableGraphReport rep = this->findMutableCycles(nullptr);
            const bool toStderr = (std::strcmp(dest, "1") == 0
                                   || std::strcmp(dest, "stderr") == 0);
            std::FILE* out = toStderr ? stderr : std::fopen(dest, "a");
            if (out) {
#if defined(__linux__)
                std::fprintf(out,
                    "protoCore PROTOCORE_MUTABLE_CYCLE_CHECK [pid %ld]: %s",
                    (proto_long) ::getpid(), rep.summary().c_str());
#else
                std::fprintf(out, "protoCore PROTOCORE_MUTABLE_CYCLE_CHECK: %s",
                             rep.summary().c_str());
#endif
                std::fflush(out);
                if (!toStderr) std::fclose(out);
            }
        }

        // Free any embedder root sets that the embedder didn't
        // explicitly destroy.  Doing this after the GC thread has
        // joined means no concurrent forEachRootSet can fire.
        {
            std::lock_guard<std::mutex> lock(rootSetsMutex_);
            for (auto* rs : rootSets_) delete rs;
            rootSets_.clear();
        }
        // P3: retire this space's entries in the process-global module root
        // table.  The table is append-only, so an entry would otherwise outlive
        // the space it names — and the allocator can hand a LATER ProtoSpace the
        // same address, whose collector would then match these entries by owner
        // and trace cells in a heap with no owner.  O(entries), at teardown only,
        // never inside a pause.  Runs after the GC thread has been joined, so no
        // walk of this space's entries can be in flight.
        globalModuleRootTable().purgeSpace(this);

        // The GC thread has joined, so nothing allocates through its context.
        delete this->gcContext;
        this->gcContext = nullptr;
        delete this->rootContext;
        freeStringInternMap(this);
        // P3: `symbolTable` is a BORROWED pointer to the process-global table
        // (globalSymbolTable()).  It must NOT be deleted: the first space to die
        // would free the table every other space of this process is still using,
        // and a single-space test suite cannot reach that use-after-free.
        //
        // `tupleInterner` below IS owned by this space and is still deleted —
        // tuple interning stays per-space (P3 D5), because a tuple's key is its
        // element ADDRESSES, which are per-space, while a symbol's key is its
        // BYTES, which are not.
        delete tupleInterner;
        tupleInterner = nullptr;
        delete impl;
        impl = nullptr;

        // Drain DirtySegment lists (live, free pool, and survivor pen)
        // and free the underlying heap nodes.  GC thread is already
        // joined above, so no concurrent access remains.
        for (auto* head : { this->dirtySegments.load(std::memory_order_relaxed),
                            this->dirtySegmentFreePool.load(std::memory_order_relaxed),
                            this->survivorPen.load(std::memory_order_relaxed) }) {
            DirtySegment* seg = head;
            while (seg) {
                DirtySegment* nextSeg = seg->next;
                delete seg;
                seg = nextSeg;
            }
        }
        this->dirtySegments.store(nullptr, std::memory_order_relaxed);
        this->dirtySegmentFreePool.store(nullptr, std::memory_order_relaxed);
        this->survivorPen.store(nullptr, std::memory_order_relaxed);

        // The free-chunk descriptors, published and pooled.  Bookkeeping, not
        // cell memory: the cells they describe stay in the retained heap
        // blocks.  Only this space's collector and threads touch them, and
        // both are gone.  Until 2.14.5 they were never deleted (LeakSanitizer
        // reported them for every destroyed space).
        {
            std::lock_guard<std::recursive_mutex> lock(globalMutex);
            for (FreeChunk* head : { this->freeChunks, this->freeChunkPool }) {
                while (head) {
                    FreeChunk* next = head->next;
                    delete head;
                    head = next;
                }
            }
            this->freeChunks = nullptr;
            this->freeChunkPool = nullptr;
        }
    }

    const ProtoObject* ProtoSpace::getResolutionChain() const {
        if (!resolutionChain_) {
            ProtoSpace* self = const_cast<ProtoSpace*>(this);
            self->resolutionChain_ = buildDefaultResolutionChain(self->rootContext);
        }
        return resolutionChain_ ? resolutionChain_->asObject(rootContext) : PROTO_NONE;
    }

    void ProtoSpace::setResolutionChain(const ProtoObject* newChain) {
        if (!newChain || newChain == PROTO_NONE) {
            resolutionChain_ = buildDefaultResolutionChain(rootContext);
            return;
        }
        const ProtoList* list = newChain->asList(rootContext);
        if (!list) {
            resolutionChain_ = buildDefaultResolutionChain(rootContext);
            return;
        }
        const proto_ulong size = list->getSize(rootContext);
        for (proto_ulong i = 0; i < size; ++i) {
            const ProtoObject* el = list->getAt(rootContext, static_cast<int>(i));
            if (!el || !el->isString(rootContext)) {
                resolutionChain_ = buildDefaultResolutionChain(rootContext);
                return;
            }
        }
        resolutionChain_ = list;
    }

    const ProtoObject* ProtoSpace::getImportModule(ProtoContext* context, const char* logicalPath, const char* attrName2create) {
        if (std::getenv("PROTO_RESOLVE_DIAG")) {
            fprintf(stderr, "TRACE: getImportModule(%s)\n", logicalPath);
        }
        return getImportModuleImpl(this, context, logicalPath, attrName2create);
    }

    // --- P3: the process-global module list, and a GC root -------------------
    //
    // The table is global — a module is loaded once for the process and found
    // once under its ModuleIdentity — while the TRACING stays where the cells
    // are: each entry records the owning space and the Phase-4 walk visits only
    // the collecting space's own entries.  See ModuleRootTable in
    // headers/proto_internal.h.

    void ProtoSpace::addModuleRoot(const ProtoObject* module) {
        globalModuleRootTable().add(module, this);
    }

    proto_ulong ProtoSpace::moduleRootCount() {
        return static_cast<proto_ulong>(globalModuleRootTable().size());
    }

    const ProtoObject* ProtoSpace::registerModule(const ModuleIdentity& id,
                                                   const ProtoObject* module) {
        if (!module || module == PROTO_NONE) return PROTO_NONE;
        // Publish-or-adopt: if this identity is already served, root and return
        // the existing module so two importers share one module, which is what
        // "a module is loaded once for the process" means.
        if (const ProtoObject* existing = sharedModuleCacheGet(id)) {
            addModuleRoot(existing);
            return existing;
        }
        sharedModuleCacheInsert(id, module);
        addModuleRoot(module);
        return module;
    }

    const ProtoObject* ProtoSpace::findModule(const ModuleIdentity& id) {
        return sharedModuleCacheGet(id);
    }

    const ProtoThread* ProtoSpace::newThread(
        ProtoContext* context,
        const ProtoString* name,
        ProtoMethod mainFunction,
        const ProtoList* args,
        const ProtoSparseList* kwargs
    ) {
        // `c` is only the allocation context of the new thread's cells. A
        // context built with no previous context registers itself as a root
        // -- the calling main thread's current context, or else
        // space->mainContext -- and a registration of this temporary context
        // detached the roots of a context still in use: a later cycle freed
        // what only that context held (NewThreadRootsTests). Restoring the
        // registration afterwards still left a window in which the allocations
        // below could park for a stop-the-world that scanned the wrong root, so
        // `c` is built never registered at all (the collector's own
        // allocation-context constructor).
        auto* c = new ProtoContext(ProtoContext::GCOwnedTag{}, this);
        auto* newThreadImpl = new(c) ProtoThreadImplementation(c, name, this, mainFunction, args, kwargs);
        // runningThreads is incremented in ProtoThreadImplementation constructor
        const ProtoThread* result = newThreadImpl->asThread(c);
        // The cells allocated here are never handed to the collector: the
        // thread leaves space->threads when it finishes, before its creator
        // joins it, and the creator holds the handle in C++ memory the
        // collector does not scan (MPSCQueueConcurrency joins a vector of
        // them). Dropping the young chain keeps them out of every cycle, as
        // they always were; destroying `c` returns the rest of its allocation
        // batch to the space (ThreadExitReleaseTests).
        c->lastAllocatedCell = nullptr;
        delete c;
        return result;
    }

    // Wait for the GC to complete a collection cycle, GC-safely.  The caller
    // holds `lock` (globalMutex).  The waiting thread leaves the stop-the-world
    // running set — so the GC quorum (parkedThreads >= runningThreads) is
    // computed without it and it cannot stall a collection — sleeps until a
    // cycle finishes (or a 50 ms watchdog fires, so a missed notify costs
    // latency, not a hang), rejoins the running set, and parks at a safepoint
    // if a stop-the-world began while it slept.  See
    // docs/archive/design-specs/2026-05-22-allocation-limit-oom-design.md.
    static void reclaimWaitLocked(ProtoSpace* space,
                                  std::unique_lock<std::recursive_mutex>& lock,
                                  ProtoContext* ctx) {
        const uint64_t startCycle =
            space->gcCycleCount.load(std::memory_order_relaxed);
        // Under the adaptive heap controller the wait ends when a cycle
        // COMPLETES, including one already in flight.  gcCycleCount counts
        // cycle starts, and a request made during a cycle is cleared at its
        // end, so waiting for a change of gcCycleCount from inside a cycle
        // lasted until the 50 ms watchdog: a stall the controller would read
        // as collection pressure.  Fixed limits keep the historical predicate.
        const bool byCompletion = adaptive::isEnabled(space);
        const uint64_t startCompleted = adaptive::cyclesCompleted(space);
        // The wait also ends when cells are published to the freelist
        // (adaptive::cellsPublished wakes one waiter per publication): the
        // sweep publishes chunks while it runs, and a thread that slept until
        // the watchdog polled the freelist every 50 ms meanwhile
        // (docs/specs/2026-10-03-collector-throughput-design.md, 4.4).
        const bool wakeOnCells = adaptive::earlyWake(space);
        // Make sure a collection will actually run.
        if (!space->gcStarted) {
            adaptive::noteCycleRequested(space);
            space->gcStarted = true;
        }
        space->gcCV.notify_all();
        const bool managed = ctx && ctx->thread;
        // Leave the running set -- of every space this OS thread belongs to,
        // when it has a thread: a thread shared by several spaces would stall
        // their collections otherwise (multispace::goOut).  A GC already parked
        // on the Phase-1 quorum must re-evaluate it, hence the notify.
        if (managed) {
            multispace::goOut(space);
        } else {
            space->runningThreads.fetch_sub(1, std::memory_order_acq_rel);
        }
        space->gcCV.notify_all();
        const auto waitStart = std::chrono::steady_clock::now();
        adaptive::waitBegin(space);
        adaptive::WakeReason reason = adaptive::WakeReason::Watchdog;
        space->memoryReclaimedCV.wait_for(
            lock, std::chrono::milliseconds(50),
            [space, startCycle, byCompletion, startCompleted, wakeOnCells, &reason] {
                if (space->state == SPACE_STATE_ENDING) {
                    reason = adaptive::WakeReason::Ending;
                    return true;
                }
                if (wakeOnCells && (space->freeChunks || space->freeCells)) {
                    reason = adaptive::WakeReason::Cells;
                    return true;
                }
                if (adaptive::cyclesCompleted(space) != startCompleted
                    || (!byCompletion
                        && space->gcCycleCount.load(std::memory_order_relaxed) != startCycle)) {
                    reason = adaptive::WakeReason::Cycle;
                    return true;
                }
                return false;
            });
        adaptive::waitEnd(space, reason);
        // Mutator stall for the adaptive heap controller (a no-op unless it
        // is enabled for this space).  globalMutex is held again here.
        adaptive::recordMutatorWait(space, static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - waitStart).count()));
#ifdef PROTOCORE_GC_INSTRUMENT
        gcprof::headroomWaitNs.fetch_add(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - waitStart).count()),
            std::memory_order_relaxed);
        gcprof::headroomWaits.fetch_add(1, std::memory_order_relaxed);
#endif
        // Rejoin the stop-the-world protocol: park if a stop-the-world began
        // while this thread was out of the running set.  Park ONLY, never
        // ProtoContext::safepoint().  This wait runs from the heap checkpoint
        // of an outermost critical section (or the depth-0 refill path), inside
        // native code that may hold a half-built structure only in C++ locals
        // and in the context's young chain -- for example a list a primitive is
        // about to turn into a tuple.  safepoint() hands that chain to the
        // collector once the context crosses maxAllocatedCellsPerContext, which
        // makes those cells candidates while nothing references them; the next
        // cycle frees them.  Young generations are submitted only when a
        // context is destroyed or at a safepoint() the embedder calls.
        if (managed) {
            // multispace::comeBack re-locks globalMutex (in the multi-space
            // case) and parks on the stop-the-world cv, so globalMutex is
            // released around it: holding it across the park would leave it
            // owned (recursive_mutex drops only one level) and wedge the GC.
            // It parks in any member space that is stopped, and drops this
            // thread's cache entries if a stop-the-world completed meanwhile
            // (the caches are not GC roots).
            lock.unlock();
            multispace::comeBack(space, ctx);
            lock.lock();
            return;
        }
        // A context without a thread rejoins UNDER globalMutex.  The collector
        // raises stwFlag and evaluates its quorum (parkedThreads >=
        // runningThreads) under globalMutex too, so the increment and the flag
        // check below are one step for it: either the quorum it evaluates
        // counts this thread, and it waits for the park below, or this thread
        // sees the flag and parks.  Done with the mutex released, the two were
        // ordered only by the atomics' memory order (an acq_rel increment
        // against a seq_cst load -- a store-load pair that order does not
        // forbid from reordering), and a collector could stop the world
        // without this thread, which then ran through it.
        space->runningThreads.fetch_add(1, std::memory_order_acq_rel);
        if (ctx && space->stwFlag.load() &&
            !(space->gcThread && std::this_thread::get_id() == space->gcThread->get_id())
            && ctx->criticalSectionDepth == 0) {
            // A context without a thread parks the same way, keyed on the
            // context's own critical-section depth.  The wait releases
            // globalMutex (held once, by the caller's `lock`) while parked.
            space->impl->parkWaiters.fetch_add(1, std::memory_order_seq_cst);
            space->parkedThreads++;
            space->gcCV.notify_all();
            multispace::waitOutStop(space, lock);   // never returns from a terminal stop
            space->impl->parkWaiters.fetch_sub(1, std::memory_order_seq_cst);
            space->parkedThreads--;
        }
    }

    void ProtoSpace::waitForHeapHeadroom(ProtoContext* ctx) {
        // No limit configured, the GC thread itself, or a contextless caller:
        // nothing to enforce — these cannot, or need not, wait for the GC.
        // Read outside globalMutex: under the adaptive controller another
        // space's growth rewrites this ceiling (recomputeCeilings, under the
        // mutex), so the read is relaxed, as every unlocked read of it is.
        if (relaxedLoad(this->maxHeapSize) <= 0 || !ctx) return;
        if (this->gcThread &&
            std::this_thread::get_id() == this->gcThread->get_id()) return;

        std::unique_lock<std::recursive_mutex> lock(globalMutex);
        int  oomStrikes      = 0;
        bool oomCallbackUsed = false;
        // Under the controller a strike is counted once per COMPLETED cycle:
        // gcCycleCount counts cycle starts, and a check made while the next
        // cycle runs would read the previous cycle's figures a second time.
        auto cyclesSeen = [this]() -> uint64_t {
            return adaptive::isEnabled(this)
                ? adaptive::cyclesCompleted(this)
                : this->gcCycleCount.load(std::memory_order_relaxed);
        };
        uint64_t lastSeenCycle = cyclesSeen();
        for (;;) {
            if (this->state == SPACE_STATE_ENDING) return;
            // Room left to grow the heap, or recycled cells already waiting in
            // the freelist — the next allocation can be served without
            // crossing the ceiling.
            if (this->heapSize < this->maxHeapSize) return;
            if (this->freeChunks || this->freeCells) return;
            // At the ceiling with an empty freelist: let the GC reclaim, then
            // re-check.  reclaimWaitLocked leaves the running set for the wait
            // so this thread never stalls the stop-the-world quorum.
            reclaimWaitLocked(this, lock, ctx);
            if (this->freeChunks || this->freeCells) return;

            // The freelist is still empty.  Distinguish "no cycle has
            // completed yet" (reclaimWaitLocked returned on its 50 ms
            // timeout) from "a cycle completed and reclaimed nothing".  Only
            // the latter is evidence of out of memory.
            uint64_t cycle = cyclesSeen();
            if (cycle == lastSeenCycle) continue;  // collection still pending
            lastSeenCycle = cycle;

            // A full cycle completed without refilling the freelist.  OOM is
            // genuine when the cycle reclaimed nothing: no future cycle can
            // do better while the live set is unchanged.  Reclamation — not
            // mark-phase reachability — is the metric, because a live
            // context's un-submitted young generation fills the heap without
            // ever entering markedList.  Two consecutive zero-reclaim cycles
            // confirm it, absorbing a single cycle's timing races.
            //
            // Under the adaptive heap controller the condition is the one of
            // its design (section 3.4, adaptive::outOfMemoryStrike): the live
            // set the cycle marked, plus every other space's heap, does not
            // fit under the process budget H less one refill batch per
            // running thread -- or the cycle reclaimed nothing at all.  The
            // cells the cycle left unreclaimed are not the test: they include
            // the threads' batches, the young generations of calls in
            // progress and the cycle's own allocations (until 2.14.3 they
            // were, and small budgets with several threads came close to a
            // false out of memory).  Two consecutive cycles confirm it, the
            // second being the "one more cycle" after which the pending
            // allocation still cannot be served.  With fixed limits
            // (setHeapLimits) the rule is unchanged.
            adaptive::OutOfMemoryInputs oom;
            const bool controllerKnows = adaptive::outOfMemoryInputs(this, oom);
            proto_ulong reclaimed =
                this->reclaimedLastCycle.load(std::memory_order_relaxed);
            const bool strike = controllerKnows ? adaptive::outOfMemoryStrike(oom)
                                                : reclaimed == 0;
            if (strike) {
                if (++oomStrikes >= 2) {
                    if (!oomCallbackUsed && this->outOfMemoryCallback) {
                        // Give the embedder one chance to free its caches.
                        oomCallbackUsed = true;
                        oomStrikes = 0;
                        lock.unlock();
                        this->outOfMemoryCallback(ctx);
                        lock.lock();
                    } else {
                        // Confirmed, unrecoverable out of memory.
                        proto_ulong live = this->liveCellsLastCycle.load(
                            std::memory_order_relaxed);
                        const int ceiling = this->maxHeapSize;
                        lock.unlock();
                        if (controllerKnows) {
                            std::fprintf(stderr,
                                "protoCore: adaptive heap hard limit %" PROTO_FMT_U " cells "
                                "(process budget) reached; live set %" PROTO_FMT_U " cells "
                                "after a full cycle, other spaces %" PROTO_FMT_U " cells, "
                                "%" PROTO_FMT_U " cells not reclaimed, last cycle reclaimed %"
                                PROTO_FMT_U " — out of memory\n",
                                oom.budgetCells, oom.liveCells, oom.otherSpacesCells,
                                oom.retainedCells, oom.reclaimedCells);
                        } else {
                            std::fprintf(stderr,
                                "protoCore: heap hard limit %d cells reached; "
                                "live set %" PROTO_FMT_U " cells, last cycle reclaimed 0 — "
                                "out of memory\n", ceiling, live);
                        }
                        std::fflush(stderr);
                        std::abort();
                    }
                }
            } else {
                oomStrikes = 0;  // the GC freed cells, or the data fits
            }
        }
    }

    // The adaptive controller's soft-zone wait at a critical-section
    // checkpoint (adaptive::softZoneCheckpoint): a refill inside a critical
    // section went past S without waiting, so the thread waits here, at depth
    // 0, for the cycle in flight.  One reclaimWaitLocked (bounded by its 50 ms
    // watchdog); the next checkpoint waits again if the cycle is still running.
    void adaptive::softZoneCheckpoint(ProtoSpace* space, ProtoContext* ctx) {
        if (!ctx || ctx->criticalSectionDepth != 0) return;
        if (space->gcThread &&
            std::this_thread::get_id() == space->gcThread->get_id()) return;
        std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        if (space->state == SPACE_STATE_ENDING) return;
        if (!adaptive::softWaitPendingFor(space)) return;
        // Cells in the freelist: the refill that comes next is served
        // without growing the heap past S, so there is nothing to wait for.
        if (adaptive::earlyWake(space) && (space->freeChunks || space->freeCells)) return;
        reclaimWaitLocked(space, lock, ctx);
    }

    Cell* ProtoSpace::getFreeCells(ProtoContext* ctx) {
        // The sweep's helper threads never allocate (spec 6.5): they hold no
        // context and take no part in stop-the-world.
        if (sweep::isHelperThread()) {
            std::fprintf(stderr, "protoCore: a collector helper thread tried to allocate cells\n");
            std::fflush(stderr);
            std::abort();
        }
        std::unique_lock<std::recursive_mutex> lock(globalMutex);
        GC_LOCK_TRACE("getFreeCells ACQ");

        const bool isGcThread = this->gcThread &&
            std::this_thread::get_id() == this->gcThread->get_id();
        // The GC thread cannot wait on its own collection, and a contextless
        // caller is not a managed mutator — both bypass the ceiling.  Ordinary
        // mutator allocations honour it, including those inside a critical
        // section: the blocking enforcement runs at critical-section
        // boundaries (ProtoContext::heapLimitCheckpoint), and the OS-request
        // clamp below keeps heapSize from crossing maxHeapSize regardless.
        // With maxHeapSize == 0 (the default) the limit is disabled and this
        // whole path is the historical unbounded allocator, bit-for-bit.
        const bool limitExempt = this->maxHeapSize <= 0 || isGcThread || !ctx;

        // One-shot soft-zone wait: the soft watermark biases toward
        // reclamation once per getFreeCells call, never blocking past it.
        bool softWaited = false;

        // Adaptive heap controller: request a cycle while there is still
        // runway before S (a no-op unless the controller is enabled).
        adaptive::pace(this);
        adaptive::noteRefill(this, ctx ? (ctx->thread ? static_cast<const void*>(ctx->thread)
                                                      : static_cast<const void*>(ctx))
                                       : nullptr);

        for (;;) {
            // Effective batch size — larger when multiple threads run so each
            // refill amortises the lock acquisition over more cells.
            int batchSize = this->blocksPerAllocation;
            if (this->runningThreads > 1) {
                int scaled = this->blocksPerAllocation
                    * static_cast<int>(this->runningThreads.load()) * 4;
                if (scaled < 60000) scaled = 60000;
                if (scaled > 65536) scaled = 65536;
                batchSize = scaled;
            }
            // The OS request below keeps this unlimited size.
            const int unlimitedBatchSize = batchSize;

            // Under a hard heap limit, a refill is capped so that all running
            // threads' batches together use at most 1/kLimitBatchFraction of
            // the limit.  A batch counts against the limit as soon as it is
            // handed out, but no cycle can reclaim the cells a thread holds,
            // so unbounded batches (up to 65,536 cells each with several
            // threads) can exhaust a small limit while the live set is far
            // below it.  Without a limit nothing changes.
            const bool limitedBatches = this->maxHeapSize > 0;
            proto_long limitCap = 0;
            if (limitedBatches) {
                const proto_long threads = std::max(1, this->runningThreads.load());
                limitCap = static_cast<proto_long>(this->maxHeapSize) / (kLimitBatchFraction * threads);
                if (limitCap < kMinLimitedBatchCells) limitCap = kMinLimitedBatchCells;
                if (batchSize > limitCap) batchSize = static_cast<int>(limitCap);
            }

            // Path #5 v2: chunked freelist — O(1) chunk pop.
            if (this->freeChunks) {
                FreeChunk* chunk = this->freeChunks;
                // Sweep publishes chunks of any size (a segment full of dead
                // cells can exceed CELL_CHUNK_SIZE), and without a limit a
                // chunk is handed out whole.  Under a limit, split only a
                // chunk larger than the cap: when the cap does not bind (a
                // generous limit) the chunk is still handed out whole.
#ifdef PROTOCORE_GC_INSTRUMENT
                const bool freshChunk = gcprof::freshChunks().count(chunk) != 0;
#endif
                if (limitedBatches && chunk->count > static_cast<proto_ulong>(limitCap)) {
                    // Hand out only batchSize cells of this chunk: cut its
                    // chain after batchSize cells and leave the remainder
                    // (same tail) on the free-chunk list.
                    Cell* batchHead = chunk->head;
                    Cell* last = batchHead;
                    for (int i = 1; i < batchSize; ++i) last = last->getNext();
                    chunk->head = last->getNext();
                    chunk->count -= static_cast<proto_ulong>(batchSize);
                    last->internalSetNextRaw(nullptr);
                    relaxedFetchAdd(this->freeCellsCount, -batchSize);
#ifdef PROTOCORE_GC_INSTRUMENT
                    (freshChunk ? gcprof::refillFreshCells : gcprof::refillRecycledCells)
                        .fetch_add(static_cast<std::uint64_t>(batchSize), std::memory_order_relaxed);
#endif
                    GC_LOCK_TRACE("getFreeCells REL(chunk-split)");
                    return batchHead;
                }
                this->freeChunks = chunk->next;
                Cell* batchHead = chunk->head;
                relaxedFetchAdd(this->freeCellsCount, -static_cast<int>(chunk->count));
#ifdef PROTOCORE_GC_INSTRUMENT
                (freshChunk ? gcprof::refillFreshCells : gcprof::refillRecycledCells)
                    .fetch_add(chunk->count, std::memory_order_relaxed);
                if (freshChunk) gcprof::freshChunks().erase(chunk);
#endif
                recycleFreeChunk(this, chunk);
                GC_LOCK_TRACE("getFreeCells REL(chunk)");
                return batchHead;
            }

            // Legacy flat freelist fallback — populated by the per-context
            // destructor return path and by OS-allocation overflow.
            if (this->freeCells) {
                if (this->freeCellsCount <= batchSize) {
                    Cell* batchHead = this->freeCells;
                    this->freeCells = nullptr;
                    this->freeCellsTail = nullptr;
#ifdef PROTOCORE_GC_INSTRUMENT
                    gcprof::refillRecycledCells.fetch_add(
                        static_cast<std::uint64_t>(std::max(0, this->freeCellsCount)), std::memory_order_relaxed);
#endif
                    relaxedStore(this->freeCellsCount, 0);
                    GC_LOCK_TRACE("getFreeCells REL(flat-all)");
                    return batchHead;
                }
                Cell* batchHead = this->freeCells;
                Cell* current = batchHead;
                int count = 1;
                while (count < batchSize) {
                    Cell* next = current->getNext();
                    if (!next) break;
                    current = next;
                    count++;
                }
                this->freeCells = current->getNext();
                current->setNext(nullptr);
                relaxedFetchAdd(this->freeCellsCount, -count);
#ifdef PROTOCORE_GC_INSTRUMENT
                gcprof::refillRecycledCells.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
#endif
                if (!this->freeCells) this->freeCellsTail = nullptr;
                GC_LOCK_TRACE("getFreeCells REL(flat-partial)");
                return batchHead;
            }

            // No free cells. Policy (2026-05-23 night): the clean
            // reaction is to refill from the OS — that is what
            // posix_memalign exists for. We do NOT trigger a GC
            // cycle here. Triggering on every freelist exhaustion,
            // independent of how full the heap is, was the historical
            // default but produced a perverse interaction with the
            // concurrent collector: the GC thread woke up,
            // immediately entered Phase-1 stop-the-world, and sat
            // there for the rest of the run because the mutator
            // workers don't park during a drain (no STW safepoint in
            // the bytecode dispatch). The Phase-1 barrier never
            // closed; mark + sweep ran exactly once at the very end
            // of the run. See
            // https://github.com/gamarino/protoST/blob/main/docs/archive/design-specs/2026-05-23-saturation-experiment.md
            // for the measurement (Phase-1 waited 2.27 s of a 2.5 s
            // run at workers=8).
            //
            // The GC is now driven exclusively by:
            //   * the soft/hard cap paths (waitForHeapHeadroom /
            //     reclaimWaitLocked) — those only kick in when a
            //     maxHeapSize is configured AND heapSize crosses
            //     softHeapLimit or maxHeapSize;
            //   * triggerGC(), the public freeRatio-driven entry
            //     point that callers can invoke explicitly;
            //   * ~ProtoSpace, which notifies on shutdown.
            // With the default maxHeapSize == 0 (no cap), getFreeCells
            // never triggers — the runtime grows by OS allocations
            // and the user gets the throughput they paid for. Set a
            // cap if you want the GC to kick in.

            // Size the OS allocation (unchanged sizing policy).  With a heap
            // limit the thread still receives only the capped batchSize; the
            // rest of the request is published as free chunks below.
            int blocksToAllocate;
            if (this->runningThreads > 1) {
                blocksToAllocate = unlimitedBatchSize;
            } else {
                blocksToAllocate = this->blocksPerAllocation * 50;
            }
            if (blocksToAllocate > kMaxBlocksPerOSAllocation)
                blocksToAllocate = kMaxBlocksPerOSAllocation;

            // --- Allocation-limit enforcement -------------------------------
            // Skipped entirely when no limit is set (maxHeapSize == 0) — then
            // the loop falls straight through to the OS-allocation path,
            // exactly as before this feature.  The exempt callers (the GC
            // thread, a contextless caller) never wait, but they are clamped
            // like everyone else: at the ceiling they get one batch, not a
            // whole OS block that would then feed the mutators.
            if (this->maxHeapSize > 0) {
                proto_long headroom = static_cast<proto_long>(this->maxHeapSize)
                              - static_cast<proto_long>(this->heapSize);
                if (headroom <= 0) {
                    // HARD zone: the heap is at its ceiling.
                    if (!limitExempt && ctx->criticalSectionDepth == 0) {
                        // A depth-0 caller holds no half-built tree, so it may
                        // block for the GC.  waitForHeapHeadroom returns once
                        // the freelist refills (or escalates a genuine OOM);
                        // re-check the freelist afterwards.
                        lock.unlock();
                        this->waitForHeapHeadroom(ctx);
                        lock.lock();
                        continue;
                    }
                    // Inside a critical section the thread cannot block — that
                    // would expose a helper's un-anchored cells to a STW
                    // cycle; nor can an exempt caller.  The critical-section entry checkpoint already
                    // enforced the limit; allow a bounded one-batch overshoot
                    // to satisfy this in-flight allocation.
                    blocksToAllocate = batchSize;
                } else {
                    if (this->softHeapLimit > 0
                        && this->heapSize >= this->softHeapLimit
                        && !limitExempt) {
                        const adaptive::SoftZone zone = adaptive::softZoneDecision(this);
                        if (zone == adaptive::SoftZone::Fixed) {
                            // SOFT zone, fixed limits: prefer reclamation over
                            // growth.  Wait one cycle, then re-check; grow only
                            // if that did not help.
                            if (!softWaited && ctx->criticalSectionDepth == 0) {
                                softWaited = true;
                                reclaimWaitLocked(this, lock, ctx);
                                continue;
                            }
                        } else if (zone == adaptive::SoftZone::Wait) {
                            // SOFT zone, adaptive controller: a cycle is
                            // requested or running.  Wait for it at depth 0
                            // (once per refill).  A caller that cannot wait
                            // (inside a critical section) or has waited
                            // already gets one batch, and its next
                            // critical-section checkpoint waits
                            // (ProtoContext::heapLimitCheckpoint).
                            if (!softWaited && ctx->criticalSectionDepth == 0) {
                                softWaited = true;
                                reclaimWaitLocked(this, lock, ctx);
                                continue;
                            }
                            adaptive::markSoftWaitPending(this);
                            if (blocksToAllocate > batchSize) blocksToAllocate = batchSize;
                        }
                        // SoftZone::Grow: no cycle pending -- after one cycle's
                        // wait the freelist is still empty: grow.
                    }
                    // Clamp the OS request so heapSize never crosses
                    // maxHeapSize.
                    if (static_cast<proto_long>(blocksToAllocate) > headroom)
                        blocksToAllocate = static_cast<int>(headroom);
                    // The partitioning below hands the first `batchSize` cells
                    // to the caller; keep batchSize within the (possibly
                    // clamped) allocation so it never indexes past the block.
                    if (batchSize > blocksToAllocate)
                        batchSize = blocksToAllocate;
                }
            }

            // --- OS allocation ----------------------------------------------
            // Do the expensive posix_memalign + chaining outside the lock so
            // other threads can make progress.
            lock.unlock();
            GC_LOCK_TRACE("getFreeCells REL(OS alloc)");

            Cell* newMemory = nullptr;
            int result = alignedArenaAlloc(reinterpret_cast<void**>(&newMemory),
                                        64,
                                        blocksToAllocate * sizeof(BigCell));
            if (result != 0) {
                // The OS itself is exhausted — beneath any protoCore ceiling.
                if (this->outOfMemoryCallback)
                    this->outOfMemoryCallback(ctx);
                std::fprintf(stderr,
                    "protoCore: OS allocation of %d cells failed (result %d) "
                    "— out of memory\n", blocksToAllocate, result);
                std::fflush(stderr);
                std::abort();
            }

            BigCell* bigCellPtr = reinterpret_cast<BigCell*>(newMemory);
            // Build the OS allocation as one contiguous chained block.
            for (int i = 0; i < blocksToAllocate - 1; ++i) {
                reinterpret_cast<Cell*>(&bigCellPtr[i])->internalSetNextRaw(
                    reinterpret_cast<Cell*>(&bigCellPtr[i+1]));
            }
            reinterpret_cast<Cell*>(&bigCellPtr[blocksToAllocate - 1])
                ->internalSetNextRaw(nullptr);

            // The first batchSize cells go directly to the calling thread.
            Cell* batchHead = reinterpret_cast<Cell*>(newMemory);
            reinterpret_cast<Cell*>(&bigCellPtr[batchSize - 1])
                ->internalSetNextRaw(nullptr);

            lock.lock();
            GC_LOCK_TRACE("getFreeCells ACQ(OS done)");
            // Atomic so the unlocked heuristic read in heapLimitCheckpoint is
            // not a data race; the value is re-validated under globalMutex.
            relaxedFetchAdd(this->heapSize, blocksToAllocate);
            retainedHeapBlocks().push_back(newMemory);
            // Process heap total, per-space ceilings under the process
            // budget, and the cycle request at the soft limit -- all no-ops
            // beyond one addition unless the adaptive controller is enabled.
            adaptive::afterHeapGrowth(this, blocksToAllocate);

            // Partition the remainder into CELL_CHUNK_SIZE chunks so the next
            // getFreeCells lands in the O(1) chunked fast path.
            int remainderStart = batchSize;
            while (remainderStart < blocksToAllocate) {
                int remaining = blocksToAllocate - remainderStart;
                int chunkSize = (remaining > static_cast<int>(CELL_CHUNK_SIZE))
                    ? static_cast<int>(CELL_CHUNK_SIZE) : remaining;
                Cell* chunkHead = reinterpret_cast<Cell*>(&bigCellPtr[remainderStart]);
                Cell* chunkTail = reinterpret_cast<Cell*>(
                    &bigCellPtr[remainderStart + chunkSize - 1]);
                chunkTail->internalSetNextRaw(nullptr);
                publishFreeChunk(this, chunkHead, chunkTail,
                                 static_cast<proto_ulong>(chunkSize));
#ifdef PROTOCORE_GC_INSTRUMENT
                gcprof::freshChunks().insert(this->freeChunks);
#endif
                remainderStart += chunkSize;
            }

#ifdef PROTOCORE_GC_INSTRUMENT
            gcprof::refillFreshCells.fetch_add(static_cast<std::uint64_t>(batchSize), std::memory_order_relaxed);
#endif
            GC_LOCK_TRACE("getFreeCells REL(return OS)");
            return batchHead;
        }
    }

    void ProtoSpace::setHeapLimits(int softCells, int hardCells) {
        std::lock_guard<std::recursive_mutex> lock(globalMutex);
        if (softCells < 0) softCells = 0;
        if (hardCells < 0) hardCells = 0;
        // A soft watermark above the hard ceiling is meaningless — clamp it.
        if (softCells > 0 && hardCells > 0 && softCells > hardCells)
            softCells = hardCells;
        // PROTOCORE_ADAPTIVE_HEAP=1 (diagnosis): the controller the
        // environment enabled at creation keeps driving this space; the
        // embedder's fixed limits are ignored.
        if (adaptive::environmentMode() == 1 && adaptive::isEnabled(this)) return;
        // Fixed limits: the adaptive controller no longer drives this space.
        adaptive::disable(this);
        // Both stored relaxed: the sweep reads them without the lock
        // (sweep::mutatorsShort).
        relaxedStore(this->softHeapLimit, softCells);
        relaxedStore(this->maxHeapSize, hardCells);
    }

    void ProtoSpace::setCollectorHelperThreads(unsigned count) {
        sweep::setHelperCount(count);
    }

    unsigned ProtoSpace::collectorHelperThreads() {
        return sweep::helperCount();
    }

    void ProtoSpace::setCollectorHelperEngagement(int mode) { sweep::setEngagement(mode); }
    int ProtoSpace::collectorHelperEngagement() { return static_cast<int>(sweep::engagement()); }
    void ProtoSpace::setSweepCursors(unsigned count) { sweep::setSweepCursors(count); }
    unsigned ProtoSpace::sweepCursors() { return sweep::sweepCursors(); }
    void ProtoSpace::setSweepPrefetch(int on) { sweep::setSweepPrefetch(on); }
    bool ProtoSpace::sweepPrefetch() { return sweep::sweepPrefetch(); }

    void ProtoSpace::enableAdaptiveHeap(const AdaptiveHeapConfig& config) {
        std::lock_guard<std::recursive_mutex> lock(globalMutex);
        adaptive::enable(this, config);
    }

    AdaptiveHeapStats ProtoSpace::adaptiveHeapStats() const {
        std::lock_guard<std::recursive_mutex> lock(globalMutex);
        return adaptive::stats(this);
    }

    proto_ulong returnUnusedCellBatch(ProtoSpace* space, Cell* head) {
        if (!space || !head) return 0;

        // Walk to the tail to get a count and a terminator.  O(batch) once per
        // exiting thread, on that thread, off every hot path: the alternative
        // is to keep a running count on ProtoThreadExtension, and that cell has
        // no spare byte left inside its 64-byte budget.
        Cell* tail = head;
        proto_ulong count = 1;
        while (Cell* next = tail->getNext()) { tail = next; ++count; }

        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        adaptive::cellsReturned(space, count);
        publishFreeChunk(space, head, tail, count);
        return count;
    }

    void ProtoSpace::submitYoungGeneration(const Cell* cell) {
        if (!cell) return;

        // Try to recycle a DirtySegment from the free pool before falling back
        // to a fresh heap allocation.  The pool is a lock-free LIFO; the GC
        // returns segments here after sweep, so steady-state submit/return is
        // alloc-free.  Single CAS on the pop, single CAS on the dirty-list
        // push — no system malloc on the hot path.
        //
        // Pops are serialized by a spin lock (pushes, done by the collector
        // after sweep, stay lock-free).  An unguarded Treiber pop reads
        // `segment->next` of a head another thread may pop and reuse at the
        // same moment, and its compare-and-swap can succeed with that stale
        // `next` if the head was pushed back meanwhile (ABA): two threads
        // then own one segment.  Found by ThreadSanitizer.  The lock is one
        // of a small process-wide array, chosen by space, so the ProtoSpace
        // layout is unchanged.
        DirtySegment* segment = nullptr;
        {
            static std::atomic_flag popLocks[16] = {};
            std::atomic_flag& popLock =
                popLocks[(reinterpret_cast<uintptr_t>(this) >> 6) & 15];
            while (popLock.test_and_set(std::memory_order_acquire)) {}
            segment = this->dirtySegmentFreePool.load(std::memory_order_acquire);
            while (segment) {
                DirtySegment* nextFree = segment->next;
                if (this->dirtySegmentFreePool.compare_exchange_weak(
                        segment, nextFree,
                        std::memory_order_acquire,
                        std::memory_order_acquire)) {
                    break;  // claimed `segment`
                }
                // A push by the collector moved the head; retry from it.
            }
            popLock.clear(std::memory_order_release);
        }
        if (!segment) {
            segment = new DirtySegment();
        }

        segment->cellChain = const_cast<Cell*>(cell);
        // Lock-free push so threads do not contend on globalMutex on every context destroy.
        segment->next = this->dirtySegments.load(std::memory_order_relaxed);
        while (!this->dirtySegments.compare_exchange_weak(
                segment->next, segment,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            // segment->next updated by compare_exchange_weak on failure
        }
    }

    void ProtoSpace::triggerGC() {
        // Public entry point, called by embedders from any thread.  The fields
        // it reads and writes (heapSize, freeCellsCount, gcStarted) are guarded
        // by globalMutex everywhere else, so it takes the lock too; the mutex
        // is recursive, so a caller that already holds it is unaffected.
        // Unlocked, these were data races (found by ThreadSanitizer).
        std::lock_guard<std::recursive_mutex> lock(globalMutex);
        double freeRatio = (this->heapSize > 0) ? (static_cast<double>(this->freeCellsCount) / this->heapSize) : 1.0;

        if (freeRatio < 0.2 || this->gcStarted) {
            if (!this->gcStarted) adaptive::noteCycleRequested(this);
            this->gcStarted = true;
            this->gcCV.notify_all();
        }
    }
}
