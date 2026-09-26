/*
 * MutableCycles.cpp -- ProtoSpace::findMutableCycles, the exact detector for
 * the one retention property protoCore's collector does not resolve on its own:
 *
 *      A CYCLE AMONG MUTABLE OBJECTS IS NEVER COLLECTED.
 *
 * The property, its proof and the rule that follows from it are in
 * docs/MemoryModel.md section 7.  The two collector sites that combine to
 * produce it are:
 *
 *   * core/ProtoSpace.cpp:519-524 -- Phase 2 adds each shard's whole
 *     ProtoSparseList to the work list as a root, UNCONDITIONALLY.  So every
 *     mutable's current value is marked whether or not anything still
 *     references its handle: the table ORIGINATES marking, it does not merely
 *     preserve values.
 *   * core/ProtoSpace.cpp:992-1000 -- Phase 5b removes an entry only once the
 *     sweep has FINALIZED its handle, i.e. only for a handle found
 *     unreachable.
 *
 * A table entry is (mutable_ref -> V) in a sparse list keyed by an integer, so
 * the table does not reference the handle H; it references V.  If V reaches H
 * then H is marked, so it is never swept, so it never finalizes, so its entry
 * is never removed, and the next cycle is bit-identical.  There is no monotone
 * progress: a later collection does not fix it.
 *
 * WHY THIS CAN BE EXACT RATHER THAN HEURISTIC.  The mutables table enumerates
 * every handle in the space -- there is nowhere for a mutable to hide -- and
 * every field of every cell is const after construction, so the only edge in
 * the whole heap that can close a loop is the handle -> state indirection the
 * table implements.  That gives the algorithm below.
 *
 * THE ALGORITHM.  Build the AUGMENTED CELL GRAPH: every cell's ordinary
 * references, PLUS one synthetic edge H_r -> V_r for each handle cell whose
 * mutable_ref r has an entry in the table.  The ordinary cell graph is acyclic
 * by construction (cells are immutable once built, so a cell can only point at
 * cells that already existed).  Therefore:
 *
 *      a cycle in the augmented cell graph  <=>  a cycle among mutables
 *
 * and one Tarjan pass over the reachable graph finds every one of them in
 * O(cells + references), with the strongly connected component itself naming
 * the participating handles.  Reporting is then a shortest closed walk inside
 * the component, so the finding arrives with a path and not only a set.
 *
 * ACYCLIC EDGES ARE NOT REPORTED, deliberately.  If H1 -> H2 with no cycle,
 * H2's liveness merely follows H1's: when H1 dies its entry is released, V1
 * dies with the next cycle, and H2's entry is released on the cycle after
 * that.  A detector that reported those would drown the real finding and would
 * be switched off, which is worse than having none.
 *
 * NOTE FOR ANYONE EXTENDING THIS.  The scan must traverse THROUGH a handle
 * cell's own fields, not stop at it.  A handle keeps the `parent` chain and
 * `attributes` it was BORN with (ProtoObjectCell holds them const;
 * ProtoObjectCell::processReferences, core/ProtoObject.cpp:527-556, reports
 * both to the collector), and `newChild(ctx, true)` puts the prototype into
 * exactly that birth chain.  So a handle-to-handle edge exists that passes
 * through no state at all, and a scan that stopped at handles would miss every
 * cycle closed by one.
 */
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace proto {

namespace {

constexpr unsigned long kDefaultCellBudget = 8000000UL;

/// How a sparse-list node was reached.  Load-bearing for SAFETY, not only for
/// pretty printing: an object's attribute list is keyed by the attribute
/// symbol's own pointer word (ProtoObject::setAttribute stores
/// `reinterpret_cast<uintptr_t>(name)`, core/ProtoObject.cpp:1086), so the key
/// of an ATTRIBUTE node can be read back as a string -- while the key of a
/// shard root is a small integer mutable_ref and the key of a user-level
/// sparse list is whatever the runtime chose.  Dereferencing one of those as a
/// ProtoString would be a wild read, so the name is recovered only where the
/// node is known to be an attribute list.
enum class NodeKind : unsigned char { Other = 0, AttributeList = 1 };

/// A reference out of a cell, with enough context to label it in a report.
struct Ref
{
    const Cell* target;
    NodeKind    targetKind;
    /// Empty for a hop with nothing worth naming (tree plumbing, the synthetic
    /// handle -> state edge, an object's `attributes` slot).
    std::string label;
};

const char* typeName(const Cell* c)
{
    if (!c) return "null";
    switch (c->getType()) {
        case CellType::Object:              return "Object";
        case CellType::List:                return "List";
        case CellType::ListSmall:           return "ListSmall";
        case CellType::Tuple:               return "Tuple";
        case CellType::String:              return "String";
        case CellType::SparseList:          return "SparseList";
        case CellType::SparseListSmall:     return "SparseListSmall";
        case CellType::Map:                 return "Map";
        case CellType::MapSmall:            return "MapSmall";
        case CellType::Set:                 return "Set";
        case CellType::Multiset:            return "Multiset";
        case CellType::MethodCell:          return "MethodCell";
        case CellType::ParentLink:          return "ParentLink";
        case CellType::Thread:              return "Thread";
        case CellType::ThreadExtension:     return "ThreadExtension";
        case CellType::ByteBuffer:          return "ByteBuffer";
        case CellType::ExternalPointer:     return "ExternalPointer";
        case CellType::ExternalBuffer:      return "ExternalBuffer";
        case CellType::LargeInteger:        return "LargeInteger";
        case CellType::Double:              return "Double";
        case CellType::StringLeafNode:      return "StringLeafNode";
        case CellType::StringInternalNode:  return "StringInternalNode";
        case CellType::MPSCQueue:           return "MPSCQueue";
        case CellType::MPSCQueueNode:       return "MPSCQueueNode";
        case CellType::MPSCQueueRetain:     return "MPSCQueueRetain";
        case CellType::TupleDictionary:     return "TupleDictionary";
        default:                            return "Cell";
    }
}

/// The mutable_ref a cell carries, or 0 when it is not a mutable's handle.
/// `getType() == Object` is the only reliable discriminator: tag 0 is shared by
/// seven cell types (see the tag-layout comment in proto_internal.h), so a bare
/// tag check would read `mutable_ref` out of a ParentLink.
unsigned long handleRefOf(const Cell* c)
{
    if (!c || c->getType() != CellType::Object) return 0;
    return static_cast<const ProtoObjectCell*>(c)->mutable_ref;
}

/// Recover an attribute key's spelling.  Only ever called for a node known to
/// belong to an object's attribute list (see NodeKind).
std::string attributeName(ProtoContext* readCtx, unsigned long key)
{
    if (key == 0) return std::string();
    ProtoObjectPointer p{};
    p.op.value = 0;
    p.oid = reinterpret_cast<const ProtoObject*>(key);
    const unsigned long tag = p.op.pointer_tag;
    const bool inlineStr = (tag == POINTER_TAG_EMBEDDED_VALUE
                            && p.op.embedded_type == EMBEDDED_TYPE_INLINE_STRING);
    if (!inlineStr && tag != POINTER_TAG_SYMBOL && tag != POINTER_TAG_STRING)
        return std::string();
    if (!readCtx) return std::string();
    std::string out;
    reinterpret_cast<const ProtoString*>(p.oid)->toUTF8String(readCtx, out);
    // Keep a report line readable: an attribute name is a name, not a document.
    if (out.size() > 64) { out.resize(61); out += "..."; }
    return out;
}

/// The scan itself.  Allocates no Cell, so it can neither add a handle to the
/// table nor trigger a collection while it walks.
class Scan
{
public:
    Scan(const ProtoSpace& space, ProtoContext* readCtx, unsigned long budget)
        : space_(space), readCtx_(readCtx), budget_(budget) {}

    MutableGraphReport run()
    {
        collectEntries();
        report_.handles = static_cast<unsigned long>(entries_.size());

        // Tarjan from every table value.  A handle cell is discovered during
        // the walk, never enumerated: the table holds no reference to it (its
        // key is an integer), which is the whole reason the property exists.
        for (const auto& e : entries_) {
            const Cell* v = ProtoObject::asCellPointer(e.second);
            if (!v) continue;
            if (index_.find(v) != index_.end()) continue;
            tarjan(v);
            if (report_.truncated) break;
        }
        report_.cellsVisited = visited_;
        std::sort(report_.cycles.begin(), report_.cycles.end(),
                  [](const MutableCycle& a, const MutableCycle& b) {
                      if (a.refs.empty() || b.refs.empty())
                          return a.refs.size() > b.refs.size();
                      return a.refs.front() < b.refs.front();
                  });
        return report_;
    }

private:
    //---------------------------------------------------------------- entries
    void collectEntries()
    {
        for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
            const ProtoSparseList* root =
                space_.mutableRoot[s].root.load(std::memory_order_acquire);
            if (!root) continue;
            walkShard(root);
        }
    }

    /// Walk a shard root without allocating.  `ProtoSparseList::processElements`
    /// would be shorter, but it builds an iterator chain of Cells
    /// (core/ProtoSparseList.cpp:347-352) and a diagnostic that allocates in
    /// order to measure allocation is a diagnostic that changes its own answer.
    void walkShard(const ProtoSparseList* root)
    {
        ProtoObjectPointer p{};
        p.oid = reinterpret_cast<const ProtoObject*>(root);
        if (p.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL) {
            const auto* small =
                toImpl<const ProtoSparseListSmallImplementation>(root);
            for (unsigned i = 0; i < ProtoSparseListSmallImplementation::MAX_INLINE; ++i) {
                if (small->keys[i] == 0 || !small->values[i]) continue;
                entries_[small->keys[i]] = small->values[i];
            }
            return;
        }
        std::vector<const ProtoSparseListImplementation*> stack;
        stack.push_back(toImpl<const ProtoSparseListImplementation>(root));
        while (!stack.empty()) {
            const ProtoSparseListImplementation* n = stack.back();
            stack.pop_back();
            if (!n) continue;
            if (!n->isEmpty && n->value) entries_[n->key] = n->value;
            if (n->previous) stack.push_back(n->previous);
            if (n->next)     stack.push_back(n->next);
        }
    }

    //--------------------------------------------------------------- children
    /// Every outgoing reference of `cell`, labelled.  Field-level knowledge
    /// rather than `Cell::processReferences` for the four types whose fields
    /// carry meaning; the generic reporter for everything else.
    void childrenOf(const Cell* cell, NodeKind kind, std::vector<Ref>& out) const
    {
        out.clear();
        switch (cell->getType()) {
            case CellType::Object: {
                const auto* oc = static_cast<const ProtoObjectCell*>(cell);
                if (oc->parent)
                    out.push_back({oc->parent, NodeKind::Other, "prototype"});
                if (oc->attributes)
                    out.push_back({oc->attributes, NodeKind::AttributeList, ""});
                // The synthetic edge that makes the whole scan work.
                if (oc->mutable_ref > 0) {
                    auto it = entries_.find(oc->mutable_ref);
                    if (it != entries_.end()) {
                        if (const Cell* v = ProtoObject::asCellPointer(it->second))
                            out.push_back({v, NodeKind::Other, ""});
                    }
                }
                return;
            }
            case CellType::ParentLink: {
                const auto* pl = static_cast<const ParentLinkImplementation*>(cell);
                if (const Cell* o = ProtoObject::asCellPointer(pl->object))
                    out.push_back({o, NodeKind::Other, ""});
                if (pl->parent)
                    out.push_back({pl->parent, NodeKind::Other, ""});
                return;
            }
            case CellType::SparseList: {
                const auto* n = static_cast<const ProtoSparseListImplementation*>(cell);
                if (const Cell* v = ProtoObject::asCellPointer(n->value)) {
                    std::string label;
                    if (kind == NodeKind::AttributeList) {
                        const std::string name = attributeName(readCtx_, n->key);
                        label = name.empty() ? std::string(".<attr>")
                                             : ("." + name);
                    }
                    out.push_back({v, NodeKind::Other, label});
                }
                if (n->previous) out.push_back({n->previous, kind, ""});
                if (n->next)     out.push_back({n->next,     kind, ""});
                return;
            }
            case CellType::SparseListSmall: {
                const auto* n = static_cast<const ProtoSparseListSmallImplementation*>(cell);
                for (unsigned i = 0; i < ProtoSparseListSmallImplementation::MAX_INLINE; ++i) {
                    if (n->keys[i] == 0) continue;
                    if (const Cell* v = ProtoObject::asCellPointer(n->values[i])) {
                        std::string label;
                        if (kind == NodeKind::AttributeList) {
                            const std::string name = attributeName(readCtx_, n->keys[i]);
                            label = name.empty() ? std::string(".<attr>")
                                                 : ("." + name);
                        }
                        out.push_back({v, NodeKind::Other, label});
                    }
                }
                return;
            }
            default: break;
        }
        // Generic: whatever the cell reports to the collector, labelled by the
        // child's own type so the path still reads.
        struct Sink { std::vector<Ref>* out; const char* containerName; };
        Sink sink{&out, typeName(cell)};
        cell->processReferences(
            const_cast<ProtoContext*>(readCtx_), &sink,
            [](ProtoContext*, void* self, const Cell* ref) {
                auto* s = static_cast<Sink*>(self);
                // Labelled by the CONTAINER's type, not the child's: the hop
                // reads as "through a List", which is what the owner needs.
                if (ref) s->out->push_back({ref, NodeKind::Other, s->containerName});
            });
    }

    //----------------------------------------------------------------- Tarjan
    struct Frame
    {
        const Cell*      cell;
        NodeKind         kind;
        std::vector<Ref> refs;
        std::size_t      next;
    };

    void tarjan(const Cell* root)
    {
        std::vector<Frame> stack;
        stack.push_back(Frame{root, NodeKind::Other, {}, 0});
        open(root);
        childrenOf(root, NodeKind::Other, stack.back().refs);

        while (!stack.empty()) {
            Frame& f = stack.back();
            if (f.next < f.refs.size()) {
                const Ref r = f.refs[f.next++];
                edgeLabel_[{f.cell, r.target}] = r.label;
                if (handleRefOf(r.target) > 0) ++report_.handleReferences;
                auto seen = index_.find(r.target);
                if (seen == index_.end()) {
                    if (visited_ >= budget_) { report_.truncated = true; return; }
                    open(r.target);
                    stack.push_back(Frame{r.target, r.targetKind, {}, 0});
                    childrenOf(r.target, r.targetKind, stack.back().refs);
                } else if (onStack_.count(r.target)) {
                    low_[f.cell] = std::min(low_[f.cell], index_[r.target]);
                }
                continue;
            }
            // All children done: close the node.
            const Cell* cell = f.cell;
            stack.pop_back();
            if (!stack.empty()) {
                const Cell* parent = stack.back().cell;
                low_[parent] = std::min(low_[parent], low_[cell]);
            }
            if (low_[cell] == index_[cell]) closeComponent(cell);
        }
    }

    void open(const Cell* c)
    {
        index_[c] = nextIndex_;
        low_[c]   = nextIndex_;
        ++nextIndex_;
        sccStack_.push_back(c);
        onStack_.insert(c);
        ++visited_;
    }

    void closeComponent(const Cell* rootCell)
    {
        std::vector<const Cell*> comp;
        while (true) {
            const Cell* c = sccStack_.back();
            sccStack_.pop_back();
            onStack_.erase(c);
            comp.push_back(c);
            if (c == rootCell) break;
        }
        // A single cell with no self-edge is not a cycle.
        if (comp.size() == 1) {
            bool selfEdge = false;
            std::vector<Ref> refs;
            childrenOf(comp[0], NodeKind::Other, refs);
            for (const Ref& r : refs) if (r.target == comp[0]) selfEdge = true;
            if (!selfEdge) return;
        }
        recordCycle(comp);
    }

    //---------------------------------------------------------------- report
    void recordCycle(const std::vector<const Cell*>& comp)
    {
        std::unordered_set<const Cell*> member(comp.begin(), comp.end());
        std::vector<unsigned long> refs;
        const Cell* start = nullptr;
        for (const Cell* c : comp) {
            const unsigned long r = handleRefOf(c);
            if (r == 0) continue;
            refs.push_back(r);
            if (!start || r < handleRefOf(start)) start = c;
        }
        std::sort(refs.begin(), refs.end());
        if (!start) start = comp[0];

        MutableCycle cyc;
        cyc.refs = refs;
        cyc.path = closedWalk(start, member);
        if (refs.empty()) {
            // No mutable handle in the component means a cycle among cells that
            // are supposed to be immutable -- a kernel invariant, not an
            // embedder rule.  Reported, because it retains just as permanently.
            cyc.path = "cycle among cells with NO mutable handle (this is a "
                       "protoCore invariant violation, not an embedder one): "
                       + cyc.path;
        }
        report_.cycles.push_back(std::move(cyc));
    }

    /// A shortest closed walk through `start`, inside the component.  A cycle
    /// with no path is not actionable.
    std::string closedWalk(const Cell* start,
                           const std::unordered_set<const Cell*>& member)
    {
        std::unordered_map<const Cell*, const Cell*> from;
        std::deque<const Cell*> queue;
        std::vector<Ref> refs;
        childrenOf(start, NodeKind::Other, refs);
        for (const Ref& r : refs) {
            if (!member.count(r.target)) continue;
            if (r.target == start) return render({start, start});
            if (from.find(r.target) == from.end()) {
                from[r.target] = start;
                queue.push_back(r.target);
            }
        }
        while (!queue.empty()) {
            const Cell* c = queue.front();
            queue.pop_front();
            childrenOf(c, NodeKind::Other, refs);
            for (const Ref& r : refs) {
                if (!member.count(r.target)) continue;
                if (r.target == start) {
                    std::vector<const Cell*> walk{start};
                    std::vector<const Cell*> back;
                    for (const Cell* x = c; x != start; x = from[x]) back.push_back(x);
                    walk.insert(walk.end(), back.rbegin(), back.rend());
                    walk.push_back(start);
                    return render(walk);
                }
                if (from.find(r.target) == from.end()) {
                    from[r.target] = c;
                    queue.push_back(r.target);
                }
            }
        }
        return "<no closed walk found inside the component>";
    }

    /// Render a walk as `#8 -[.__class__]-> #3 -[.instances > List]-> #8`:
    /// handles are the waypoints, and the labelled hops between them are what
    /// the owner has to go and change.
    std::string render(const std::vector<const Cell*>& walk)
    {
        std::string out;
        std::vector<std::string> pending;
        bool started = false;
        for (std::size_t i = 0; i < walk.size(); ++i) {
            const Cell* c = walk[i];
            const unsigned long r = handleRefOf(c);
            if (i > 0) {
                auto it = edgeLabel_.find({walk[i - 1], c});
                if (it != edgeLabel_.end() && !it->second.empty())
                    pending.push_back(it->second);
            }
            const bool isWaypoint = (r > 0) || i == 0 || i + 1 == walk.size();
            if (!isWaypoint) continue;
            std::string node = (r > 0) ? ("#" + std::to_string(r))
                                       : (std::string(typeName(c)) + "@state");
            if (!started) { out = node; started = true; pending.clear(); continue; }
            out += " -[";
            for (std::size_t k = 0; k < pending.size(); ++k) {
                if (k) out += " > ";
                out += pending[k];
            }
            if (pending.empty()) out += "...";
            out += "]-> ";
            out += node;
            pending.clear();
        }
        return out;
    }

    struct PairHash
    {
        std::size_t operator()(const std::pair<const Cell*, const Cell*>& p) const
        {
            return std::hash<const void*>()(p.first) * 31u
                 ^ std::hash<const void*>()(p.second);
        }
    };

    const ProtoSpace& space_;
    ProtoContext*     readCtx_;
    unsigned long     budget_;

    std::unordered_map<unsigned long, const ProtoObject*> entries_;
    std::unordered_map<const Cell*, unsigned long>        index_;
    std::unordered_map<const Cell*, unsigned long>        low_;
    std::unordered_set<const Cell*>                       onStack_;
    std::vector<const Cell*>                              sccStack_;
    std::unordered_map<std::pair<const Cell*, const Cell*>, std::string, PairHash>
                                                          edgeLabel_;
    unsigned long     nextIndex_ = 1;
    unsigned long     visited_   = 0;
    MutableGraphReport report_;
};

}  // namespace

std::string MutableGraphReport::summary() const
{
    // Always carries its numbers, on a clean result as much as on a finding: a
    // report whose counters are all zero is a broken scan that looks like a
    // clean graph.
    std::string s = "mutable-cycle scan: " + std::to_string(handles)
                  + " handles in the mutables table, " + std::to_string(handleReferences)
                  + " references to a mutable handle, " + std::to_string(cellsVisited)
                  + " cells walked, "
                  + (truncated ? "TRUNCATED (cell budget exhausted -- a cycle "
                                 "reported below is still real, but the ABSENCE "
                                 "of others is NOT established)"
                               : "complete")
                  + "; " + std::to_string(cycles.size()) + " cycle(s)\n";
    for (const MutableCycle& c : cycles) {
        // A component of hundreds of handles would print an unreadable line, so
        // the list is capped and the total is stated.  The total is the number
        // that matters: it is how many table entries the cycle holds for ever.
        constexpr std::size_t kMaxRefsShown = 12;
        s += "  CYCLE " + std::to_string(c.refs.size()) + " handle(s) {";
        for (std::size_t i = 0; i < c.refs.size() && i < kMaxRefsShown; ++i) {
            if (i) s += ",";
            s += std::to_string(c.refs[i]);
        }
        if (c.refs.size() > kMaxRefsShown) s += ",...";
        s += "}: " + c.path + "\n";
    }
    return s;
}

MutableGraphReport ProtoSpace::findMutableCycles(ProtoContext* context,
                                                 unsigned long cellBudget) const
{
    ProtoContext* readCtx = context ? context : this->rootContext;
    const unsigned long budget = cellBudget ? cellBudget : kDefaultCellBudget;

    // The critical section is what makes the walk safe, and it is the reason
    // this method takes a context at all.  A thread inside a critical section
    // does not park for stop-the-world (ProtoContext::parkForStopTheWorld skips
    // while criticalSectionDepth > 0), so the quorum cannot be met and no NEW
    // cycle can begin while the scan holds pointers into the heap.  A cycle
    // already in flight cannot free anything the scan can see either:
    // everything reachable from the LIVE mutables table was either marked at
    // that cycle's stop-the-world or allocated after it, and neither is a
    // candidate of that cycle (docs/GarbageCollector.md, "Why the release is
    // sound").
    //
    // `context == nullptr` skips it, for the one caller that does not need it:
    // the collector's own thread, which is the only thread that can start a
    // stop-the-world and therefore excludes one by running.
    if (context) {
        ProtoContext::CriticalSection cs(context);
        Scan scan(*this, readCtx, budget);
        return scan.run();
    }
    Scan scan(*this, readCtx, budget);
    return scan.run();
}

}  // namespace proto
