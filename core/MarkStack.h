/*
 * MarkStack.h — the collector's mark-phase work structures.
 *
 * Internal to protoCore (not installed).  docs/GarbageCollector.md,
 * section "Mark work list", describes the algorithm that uses them.
 *
 * Two structures live outside the cell heap during a cycle:
 *
 *   * the mark stack: the grey cells, marked and waiting to have their
 *     references traced.  A cell is marked when it is pushed, so an entry
 *     is never a duplicate and the stack holds at most one entry per live
 *     cell.  Its capacity is FIXED (kDefaultMarkStackEntries); a push that
 *     finds it full leaves the cell unmarked and raises the overflow flag,
 *     and the collector recovers by rescanning (see ProtoSpace.cpp, the
 *     mark phase).  Its memory is therefore bounded whatever the heap's
 *     shape.
 *
 *   * the marked list: every cell marked this cycle, kept for the bulk
 *     unmark after sweep and for the overflow rescan.  One entry per live
 *     cell (8 bytes per 64-byte cell, 1/8 of the live heap at most), held
 *     in fixed-size blocks so it grows without copying and with at most one
 *     block of slack.
 *
 * Both are owned by the collector thread and touched by no other thread.
 */

#ifndef PROTOCORE_MARKSTACK_H
#define PROTOCORE_MARKSTACK_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace proto {

class Cell;

namespace gcmark {

    /** Default capacity of the mark stack, in entries (8 MiB). */
    constexpr std::size_t kDefaultMarkStackEntries = std::size_t(1) << 20;

    /**
     * A list of cell pointers stored in blocks of kBlockEntries.  Growing
     * never copies the entries already stored.  `reset` empties it and keeps
     * up to kRetainedBlocks blocks for the next cycle, freeing the rest.
     */
    class CellBlockList {
    public:
        static constexpr std::size_t kBlockEntries = std::size_t(1) << 15;  // 256 KiB
        static constexpr std::size_t kRetainedBlocks = 4;                    // 1 MiB

        void push_back(const Cell* cell) {
            const std::size_t offset = size_ & (kBlockEntries - 1);
            if (offset == 0 && (size_ >> kBlockShift) == blocks_.size()) addBlock();
            blocks_[size_ >> kBlockShift][offset] = cell;
            ++size_;
        }
        const Cell* operator[](std::size_t i) const {
            return blocks_[i >> kBlockShift][i & (kBlockEntries - 1)];
        }
        std::size_t size() const { return size_; }
        /** Bytes of the blocks currently allocated. */
        std::size_t reservedBytes() const {
            return blocks_.size() * kBlockEntries * sizeof(const Cell*);
        }
        template <typename F> void forEach(F&& f) const {
            std::size_t remaining = size_;
            for (const auto& block : blocks_) {
                if (remaining == 0) break;
                const std::size_t n = remaining < kBlockEntries ? remaining : kBlockEntries;
                for (std::size_t i = 0; i < n; ++i) f(block[i]);
                remaining -= n;
            }
        }
        void reset() {
            size_ = 0;
            if (blocks_.size() > kRetainedBlocks) blocks_.resize(kRetainedBlocks);
        }

    private:
        static constexpr unsigned kBlockShift = 15;
        static_assert((std::size_t(1) << kBlockShift) == kBlockEntries, "block shift");
        void addBlock() {
            blocks_.emplace_back(new const Cell*[kBlockEntries]);
        }
        std::vector<std::unique_ptr<const Cell*[]>> blocks_;
        std::size_t size_ = 0;
    };

    /**
     * The mark stack: a fixed-capacity LIFO of cell pointers.  The buffer is
     * allocated on first use and reused by every later cycle of the same
     * collector thread; only the pages a cycle actually reaches are touched.
     */
    class MarkStack {
    public:
        explicit MarkStack(std::size_t capacity) : capacity_(capacity) {}
        bool push(const Cell* cell) {
            if (top_ == capacity_) return false;
            if (!buffer_) buffer_.reset(new const Cell*[capacity_]);
            buffer_[top_++] = cell;
            if (top_ > peak_) peak_ = top_;
            return true;
        }
        bool empty() const { return top_ == 0; }
        const Cell* pop() { return buffer_[--top_]; }
        const Cell* top() const { return buffer_[top_ - 1]; }
        std::size_t capacity() const { return capacity_; }
        std::size_t peak() const { return peak_; }
        /** Start a cycle; a capacity change (tests only) drops the buffer. */
        void beginCycle(std::size_t capacity) {
            top_ = 0;
            peak_ = 0;
            if (capacity != capacity_) {
                buffer_.reset();
                capacity_ = capacity;
            }
        }

    private:
        std::unique_ptr<const Cell*[]> buffer_;
        std::size_t capacity_;
        std::size_t top_ = 0;
        std::size_t peak_ = 0;
    };

    /** What the mark phase of the last completed cycle did. */
    struct MarkStats {
        std::uint64_t cycles = 0;            // cycles recorded since start-up
        std::size_t peakStackEntries = 0;    // deepest the mark stack got
        std::size_t stackCapacity = 0;       // its capacity that cycle
        std::size_t overflowRescans = 0;     // rescans the overflow forced
        std::size_t youngCellsWalked = 0;    // young-chain cells visited
        std::size_t markedCells = 0;         // cells marked (the live set)
        std::size_t markedListBytes = 0;     // blocks the marked list held
    };

    /** Record the mark phase of a completed cycle (collector thread). */
    void recordMarkStats(const MarkStats& stats);
    /** The mark phase of the last completed cycle of any space. */
    MarkStats lastMarkStats();
    /** The mark stack's capacity for the next cycles, in entries. */
    std::size_t markStackCapacity();
    /**
     * Tests only: set the mark stack's capacity (entries, at least 1);
     * 0 restores kDefaultMarkStackEntries.  Takes effect at the next cycle.
     */
    void setMarkStackCapacityForTest(std::size_t entries);

}  // namespace gcmark
}  // namespace proto

#endif
