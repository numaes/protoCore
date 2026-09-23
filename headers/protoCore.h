/*
 * protoCore
 *
 *  Created on: November, 2017 - Redesign January, 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */

#ifndef PROTO_H_
#define PROTO_H_

#include <atomic>
#include <compare>
#include <condition_variable>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace proto
{
    class SymbolTable;  // forward declaration for 64-shard interning table
    class TupleInterner;  // forward declaration for the tuple interning table
    struct MutableValueCacheEntry;  // defined in proto_internal.h

    // Forward declarations
    class ProtoStringIterator;
    class ProtoTupleIterator;
    class Cell;
    class BigCell;
    class ProtoContext;
    class ProtoSpace;
    class ProtoRootSet;
    class DirtySegment;
    class ProtoObject;
    class TupleDictionary;
    class ProtoTuple;
    class ProtoString;
    class ProtoExternalPointer;
    class ProtoExternalBuffer;
    class ParentLink;
    class ProtoList;
    class ProtoListIterator;
    class ProtoSparseList;
    class ProtoSparseListImplementation;  // raw AVL impl, internal
    class ProtoSparseListIterator;
    class ProtoMap;
    class ProtoMapIterator;
    class ProtoSet;
    class ProtoSetIterator;
    class ProtoMultiset;
    class ProtoMultisetIterator;
    class ProtoObjectCell;
    class ProtoByteBuffer;
    class ProtoThread;
    class ProtoSpaceImplementation;
    class ModuleProvider;
    class ProviderRegistry;

    //! Useful constants.
    //! @warning They should be kept in sync with proto_internal.h!
    #define PROTO_TRUE ((const proto::ProtoObject*)  1217UL) // Tag: EMBEDDED_VALUE (1), Type: BOOLEAN (3), Value: 1
    #define PROTO_FALSE ((const proto::ProtoObject*) 193UL)  // Tag: EMBEDDED_VALUE (1), Type: BOOLEAN (3), Value: 0
    #define PROTO_NONE ((const proto::ProtoObject*)  321UL)  // Tag: EMBEDDED_VALUE (1), Type: NONE (5), Value: 0

    typedef const ProtoObject*(*ProtoMethod)(
        ProtoContext* context,
        const ProtoObject* self,
        const ParentLink* parentLink,
        const ProtoList* positionalParameters,
        const ProtoSparseList* keywordParameters
    );

    class ProtoObject
    {
    public:
        //- Object Model
        const ProtoObject* getPrototype(ProtoContext* context) const;
        /**
         * @brief Copy the receiver into a new object carrying the same own
         *        attributes and the same parents.
         *
         * The clone is a SIBLING, not a child: it shares the receiver's
         * parent chain, and the receiver is NOT one of its parents — use
         * `newChild` for that.  Own attributes come across by structural
         * sharing of the immutable attribute tree, so the copy is cheap and
         * the two objects never see each other's later writes.
         *
         * `isMutable` selects the form of the COPY, independently of the
         * receiver: `false` (the default) yields an immutable object, `true`
         * an independently mutable one.  This is the supported freeze / thaw
         * operation.
         *
         * A mutable receiver is read through its CURRENT snapshot, so the
         * clone carries the attributes and parents the object holds now, not
         * the ones it was created with.
         *
         * Returns PROTO_NONE when the receiver is not an object cell:
         * integers, strings, lists and other tagged primitives have no
         * attribute table to copy.
         */
        const ProtoObject* clone(ProtoContext* context, bool isMutable = false) const;
        /**
         * @brief Creates a new object whose immediate parent is `this`.
         *
         * The new object's own chain is `{this} ∪ this's own chain at the
         * moment of THIS call` — captured BY VALUE, once, here. `this` is
         * resolved to its CURRENT snapshot first when it is mutable (the
         * handle cell's own `parent` field is fixed at `newObject(true)`
         * time and never updated in place — mutation publishes a fresh
         * state into the mutable shard instead — so reading it directly
         * would silently drop every ancestor `this` gained afterwards via
         * `addParent`/`setParents`).
         *
         * A consequence of capturing by value: a LATER re-parenting of
         * `this` (e.g. `cls->setParents(ctx, [newBase])` after
         * `inst = cls->newChild(ctx)` already exists) is NOT retroactively
         * seen by `inst`, at any remove — only by children created AFTER
         * the re-parenting see it, whether `this` is `inst`'s direct
         * class or a more distant ancestor re-parented later. See
         * CHANGELOG.md for the full history, including how this relates
         * to protoST's `addBehavior:` mechanism.
         */
        const ProtoObject* newChild(ProtoContext* context, bool isMutable = false) const;

        //- Attributes
        /**
         * @brief Looks up `name`, in this object's own attributes first,
         * then along its parent chain (own chain first, head to tail).
         *
         * **No depth cap** — an earlier revision gave up
         * (`iterationCount > 500`) and returned `PROTO_NONE` ("not
         * found") for a receiver whose own chain was longer than 500
         * entries, a false negative for a perfectly good hierarchy, the
         * same class of bug `isInstanceOf`'s and `hasAttribute`'s old
         * caps had. No lookup or traversal method in protoCore has a
         * depth cap any more (`isInstanceOf`, `hasParent`, `hasAttribute`,
         * `getAttributes`, and this method): they all now always agree
         * with each other, at any depth.
         *
         * Termination without a cap is guaranteed by construction, not by
         * a limit: this walk — like every chain-lookup method — only ever
         * follows ONE receiver's own, already-built `ParentLinkImplementation`
         * list (built once, forward only, by `newChild`/`addParent`/
         * `setParents`, and never mutated afterward), and it never follows
         * a visited entry into THAT entry's own separate chain — so it is
         * always a single forward pass over one strictly finite list,
         * regardless of what any OTHER object's chain happens to
         * reference (see `setParents` below for the limits of what that
         * method's own self-reference check catches, and why a case it
         * does not catch still cannot make this walk loop).
         */
        const ProtoObject* getAttribute(ProtoContext* context, const ProtoString* name, bool callbacks = true) const;
        /**
         * @brief Is `name` present, own or inherited (`attr = None` still
         * counts as present, distinct from absent)?
         *
         * Same shape as `getAttribute`'s chain-navigation loop — own
         * attributes first, then the linearised chain, head to tail — a
         * pure linear scan, allocation-free, no recursion, no depth cap
         * (see `getAttribute`'s doc comment for why none is needed).
         * Resolves a mutable receiver (and every mutable object visited
         * along the chain) to its current snapshot, exactly as
         * `getAttribute` does, so an instance of a mutable class sees
         * whatever ancestors the class currently has (or had at the
         * instance's own creation, for `newChild` — see its doc comment).
         *
         * Always agrees with `getAttribute` (neither has a cap any more)
         * and with `getAttributes`' merged view, at any depth.
         */
        const ProtoObject* hasAttribute(ProtoContext* context, const ProtoString* name) const;
        const ProtoObject* hasOwnAttribute(ProtoContext* context, const ProtoString* name) const;
        const ProtoObject* setAttribute(ProtoContext* context, const ProtoString* name, const ProtoObject* value) const;
        /**
         * @brief Atomic compare-and-swap on an own-attribute.
         *
         * Writes `newValue` to `name` only if the receiver's current OWN
         * value for `name` is still (pointer-)identical to `expected`, and
         * reports whether the swap happened. This exposes the shard-root CAS
         * loop `setAttribute` already runs internally so embedders can build
         * lock-free read-modify-write sequences (e.g. appending to a list
         * held under an attribute) without an external mutex:
         *
         *     for (;;) {
         *         old = obj->getOwnAttributeDirect(ctx, key);
         *         neu = ... derive from old ...;
         *         if (obj->setAttributeIfEqual(ctx, key, old, neu)) break;
         *     }
         *
         * `expected == nullptr` means "the attribute is currently absent" —
         * the swap then installs `name` only if no own-value exists yet.
         *
         * The comparison is against the OWN attribute value only; parent
         * (prototype-chain) values are never consulted. Read `expected` with
         * an own-attribute read (`getOwnAttributeDirect`) so a CAS failure
         * reflects a genuine concurrent write rather than a chain mismatch.
         *
         * Requires a MUTABLE receiver: an immutable object cannot be updated
         * in place, so the call is a no-op returning `false`. Returns `false`
         * (without writing) when the receiver is not an object cell.
         *
         * @return `true` if `newValue` was installed; `false` if the current
         *         own-value was not `expected`, or the receiver is immutable
         *         / not an object.
         */
        bool setAttributeIfEqual(ProtoContext* context, const ProtoString* name,
                                 const ProtoObject* expected,
                                 const ProtoObject* newValue) const;
        /**
         * Remove an own-attribute from the object.  Mirrors `setAttribute`'s
         * mutable/immutable contract:
         *
         *   - **Immutable** receivers return a new ProtoObject* whose
         *     attribute table no longer carries `name`.  The original is
         *     untouched.
         *   - **Mutable** receivers update the shard root in place via the
         *     same CAS loop used by `setAttribute` and return `this`.
         *
         * If `name` is not present as an OWN attribute (the chain may still
         * resolve it from a parent), the call is a no-op and returns `this`
         * without allocating.  Use `hasOwnAttribute` first if you need to
         * know whether the receiver carried the name.
         *
         * Removal is at the OWN level only — parent attributes are NEVER
         * affected, so `del child.x` on an instance whose class still
         * defines `x` leaves the class binding intact (CPython semantics).
         */
        const ProtoObject* removeAttribute(ProtoContext* context, const ProtoString* name) const;
        /**
         * @brief The merged view: every attribute reachable from this
         * object, own or inherited.
         *
         * **Merge order (shadowing rule)**: own attributes first, then
         * this object's own FLATTENED chain, head to tail. A key already
         * set by a nearer entry is never overwritten by a farther one —
         * own attributes win over every ancestor's, and among ancestors a
         * NEARER one (earlier in the chain) wins over a FARTHER one. This
         * is exactly `getAttribute`'s/`hasAttribute`'s own precedence
         * rule (first match wins, walking own-then-chain in the same
         * order), so all three always agree on which value a given key
         * resolves to.
         *
         * This walks the receiver's own chain directly (like
         * `getAttribute`/`hasAttribute`/`isInstanceOf`) rather than
         * recursing into just the first parent link and stopping there —
         * an object with more than one DIRECT parent (an `addParent`-built
         * diamond, or a `setParents` list with more than one entry) has
         * every one of them contribute its attributes, not just the
         * first. No depth cap (see `getAttribute`'s doc comment for why
         * none is needed): the chain is walked in full.
         *
         * A mutable receiver — and every mutable object visited along the
         * chain — is resolved to its current snapshot, so the merge
         * reflects each object's current version.
         */
        const ProtoSparseList* getAttributes(ProtoContext* context) const;
        const ProtoSparseList* getOwnAttributes(ProtoContext* context) const;
        /**
         * Returns the value of an own-attribute by interned symbol pointer key, or nullptr if not
         * found. Resolves mutable state once internally. Does NOT traverse the prototype chain and
         * does NOT invoke descriptor protocol — use only for plain instance own-attribute reads
         * where the caller guarantees name is a POINTER_TAG_SYMBOL (e.g. co_names entries).
         */
        const ProtoObject* getOwnAttributeDirect(ProtoContext* context, const ProtoString* name) const;
        /**
         * @brief Walk the receiver's OWN attributes as (name, value) pairs.
         *
         * Attribute keys are stored as the interned symbol pointer
         * reinterpreted as an integer, so the ProtoSparseList returned by
         * `getOwnAttributes` has opaque integers for keys: an embedder gets
         * the values but cannot recover the names.  This walk is the
         * supported way to enumerate the names.
         *
         * `method` is invoked once per own attribute with the canonical
         * symbol for the name and the value stored under it.  The name is
         * the very pointer the attribute was set with, so it compares equal
         * by identity to the symbol the embedder holds, and passing it back
         * to `getAttribute` / `getOwnAttributeDirect` returns the value the
         * callback received.  (A name may be either a heap symbol or an
         * inline string — `ProtoString` handles both — so use the string API
         * on it and never dereference it as a Cell.)
         *
         * Only OWN attributes are visited; the prototype chain is never
         * traversed — use `getAttributes` for the merged view.  PROTO_NONE
         * is a value like any other and IS reported; an attribute that was
         * removed is not.  The mutable snapshot is resolved once, exactly as
         * `getAttribute` resolves it, so a mutable receiver is walked as it
         * stood when the call started; mutating the receiver from the
         * callback is allowed and does not change what the rest of the walk
         * reports.
         *
         * **The order in which attributes are visited is unspecified** and
         * may change between releases.  Do not rely on it.
         *
         * The walk allocates nothing — it creates no Cell, so
         * `ProtoContext::allocatedCellsCount` is unchanged by it — and runs
         * `method` OUTSIDE any GC critical section, so the callback may
         * allocate, call `safepoint()`, park for a collection and run
         * arbitrary embedder code.  For the duration of the walk the
         * snapshot is anchored in `ProtoContext::pendingRoot` (saved and
         * restored around the call), which is what keeps the attribute tree
         * reachable while the callback runs.
         *
         * Non-object receivers (integers, strings, ...) have no own
         * attributes: the callback is simply never invoked.
         */
        void processOwnAttributes(ProtoContext* context, void* self,
                                  void (*method)(ProtoContext*, void*, const ProtoString*, const ProtoObject*)) const;

        //- Inheritance
        const ProtoList* getParents(ProtoContext* context) const;
        /**
         * @brief Returns the immediate (first) parent without allocating a list.
         *
         * Equivalent to `getParents(ctx)->getAt(ctx, 0)` but without the
         * ProtoList allocation per call, and correctly resolves mutable
         * objects to their current snapshot (which `getPrototype` does not).
         *
         * Returns `PROTO_NONE` when the object has no parent.  This is the
         * recommended hot-path API for embedders walking single-inheritance
         * chains (e.g. `type(obj)` lookups in protoPython, where allocating
         * a ProtoList per attribute access dominates wall time).
         */
        const ProtoObject* getFirstParent(ProtoContext* context) const;
        /**
         * @brief Is `target` `this` itself, or a direct entry in this
         * object's own (single-level) parent chain?
         *
         * Allocation-free: walks `oc->parent` directly instead of building
         * a `ProtoList` via `getParents()`. No step limit.
         *
         * This is a single-level scan of the receiver's own chain — but
         * `newChild`, `addParent` and `setParents` all guarantee that an
         * object's own chain already contains every one of its ancestors
         * as a direct entry, so in practice this answers the full,
         * transitive "is-ancestor" question, including for an object
         * created (via `newChild`) from a MUTABLE prototype that was
         * re-parented AFTER that object's creation: `newChild` resolves
         * the prototype's CURRENT snapshot when capturing the child's
         * chain, so the child's own chain already carries whatever
         * ancestors the prototype had as of the child's creation. (An
         * object created BEFORE a later re-parenting does not
         * retroactively gain the new ancestor, by design — see
         * `newChild`.)
         *
         * Agrees with `isInstanceOf` (modulo the `target == this` case,
         * which `isInstanceOf` does not special-case — an object is not
         * its own instance) and with what `getAttribute` finds, at any
         * depth — neither has a depth cap.
         *
         * A mutable receiver is resolved to its current snapshot first, so
         * the answer reflects the object's current version's chain.
         */
        int hasParent(ProtoContext* context, const ProtoObject* target) const;
        /**
         * @brief Adds `newParent` (and, transitively, every one of ITS OWN
         * ancestors not already present) as a parent of this object,
         * flattened, prepended in front of the existing chain.
         *
         * **Ordering differs from `setParents`.** For a SINGLE call this
         * inserts, in order: `newParent`, then `newParent`'s own ancestors
         * (in their own chain order) not already present, all prepended in
         * front of whatever chain `this` already had. Calling `addParent`
         * MULTIPLE times therefore INTERLEAVES each call's own ancestors
         * immediately after that call's parent and before the PREVIOUS
         * call's block: `d->addParent(ctx, b); d->addParent(ctx, c);`
         * (with `b`'s own ancestor `ba` and `c`'s own ancestor `ca`, both
         * not already present) yields `[c, ca, b, ba, ...]` — `ca`
         * appears BEFORE `b`.
         *
         * `setParents(ctx, [c, b])` with the SAME ancestries instead
         * appends ALL listed parents FIRST, then ALL of their missing
         * ancestors AFTER, in listed-parent order: `[c, b, ca, ba]` — `ca`
         * appears AFTER `b`. Since attribute lookup walks the chain head
         * to tail and the first match wins, this changes attribute
         * PRECEDENCE whenever `ca` (or `ba`) also defines an attribute `b`
         * (or `c`) itself defines: `addParent`-built chains let a parent's
         * OWN ancestor shadow a LATER-added parent for that attribute;
         * `setParents`-built chains never let any listed parent's ancestor
         * shadow another LISTED parent — only another ancestor.
         */
        const ProtoObject* addParent(ProtoContext* context, const ProtoObject* newParent) const;
        const ProtoObject* addParentInternal(ProtoContext* context, const ProtoObject* newParent) const;
        /**
         * @brief Replace the entire parent chain with `newParents`, FLATTENED.
         *
         * Use this when an embedder needs to mutate the prototype
         * chain wholesale — e.g. when a user-language `__bases__`
         * reassignment must drop the old bases entirely instead of
         * just appending new ones.
         *
         * **Behaviour change**: the resulting chain is no longer exactly
         * `newParents` installed verbatim. It is, in order: (1) the entries
         * of `newParents`, in the given order, de-duplicated; (2) every
         * ancestor of each of those listed parents — walking each parent's
         * own chain in that parent's own order, in the same order the
         * parents were listed — that is not already present. This gives
         * `setParents` the same invariant `newChild`/`addParent` already
         * guarantee (an object's own chain always contains every one of
         * its ancestors as a direct entry), so `getAttribute`,
         * `isInstanceOf` and `hasParent` all see the same ancestor set for
         * this object (none of the three has a depth cap). A
         * list that already contains every ancestor of every listed parent
         * (e.g. a full linearization) is unaffected by step 2 and installs
         * exactly as given, in the exact same order — a no-op relative to
         * the old verbatim behaviour.
         *
         * **Ordering differs from `addParent`'s** whenever a listed
         * parent's own ancestor is not already present: `setParents`
         * appends ALL missing ancestors AFTER ALL listed parents (step 2
         * runs after step 1 completes for every entry), while `addParent`
         * interleaves each call's own missing ancestors immediately after
         * that call's parent. See `addParent`'s doc comment for a worked
         * example — the difference changes attribute lookup precedence
         * whenever a listed parent and another listed parent's ancestor
         * both define the same attribute name.
         *
         * - For an immutable object, returns a freshly-built handle
         *   sharing the same attributes but with the rebuilt parent
         *   chain.  The original handle becomes stale.
         * - For a mutable object, updates the per-shard mutable state
         *   in place via a CAS loop and returns the SAME handle
         *   (mirroring `addParent` and `setAttribute`).
         *
         * The list is interpreted with index 0 as the immediate
         * (first) parent, matching `getParents()`'s output order.
         * Passing an empty or null list clears the parent chain
         * entirely.
         *
         * **Self-reference is a silent no-op**, not an error: an entry
         * that would make this object its own ancestor — a listed parent,
         * or an ancestor reached while flattening one, equal to this
         * object itself — is simply OMITTED, and every other entry is
         * still applied normally. This can only ever happen for a MUTABLE
         * receiver: its handle is stable across mutation, so it is the
         * only case where an earlier `setParents` call on another mutable
         * object could already have captured a reference back to it (e.g.
         * two mutable objects `setParents`'d at each other). An immutable
         * call always builds a brand-new handle nothing could have
         * referenced yet, so it can never become its own ancestor and
         * nothing is ever omitted on that account. This matches
         * `addParent`, which already tolerates `obj->addParent(ctx, obj)`
         * as a no-op.
         *
         * This check catches a DIRECT reference back to the receiver only
         * — a listed parent, or an ancestor found while walking a LISTED
         * parent's own (one-level) chain. It does not, and cannot without
         * doing unbounded work, catch a longer chain of references built
         * up across several SEPARATE `setParents` calls on different
         * mutable objects (e.g. `a.setParents(ctx,[b])`, then
         * `b.setParents(ctx,[c])`, then `c.setParents(ctx,[a])` — none of
         * these three calls omits anything). This is not a safety gap:
         * no lookup method (`getAttribute`, `hasAttribute`, `isInstanceOf`,
         * `hasParent`, `getAttributes`) ever follows a visited chain entry
         * into THAT entry's own separate chain — every one of them walks
         * only the single, already-built, immutable list it started on —
         * so data shaped like the above never causes a hang, a crash, or
         * an incorrect answer; it just is not detected or rejected here.
         */
        const ProtoObject* setParents(ProtoContext* context, const ProtoList* newParents) const;
        /**
         * @brief Is `prototype` a transitive ancestor of this object?
         *
         * Returns `PROTO_TRUE` when found, `PROTO_NONE` when not (never a
         * third value — earlier revisions of this method gave up on a very
         * deep chain and returned `PROTO_FALSE`; that arbitrary 50-step cap
         * is gone, along with the fixed-size sibling stack and the
         * allocation it required).
         *
         * A pure, allocation-free linear scan of the receiver's own chain —
         * the same one `getAttribute` walks, and, like it, with no depth
         * cap — with no recursion. This scan alone is sufficient and exact
         * because `newChild`, `addParent` AND
         * `setParents` all guarantee that an object's own chain already
         * contains every one of its ancestors as a direct entry — there is
         * no construction path that leaves an ancestor reachable only
         * through a visited parent's own separate chain, so no recursive
         * probe is needed.
         *
         * An object with NO parent chain of its own (never `newChild`'d or
         * `addParent`'d/`setParents`'d anything) still answers `PROTO_TRUE`
         * for `space->objectPrototype` — the same universal-root fallback
         * `getPrototype` applies for a parentless object cell — UNLESS the
         * receiver IS `objectPrototype` itself (which is not its own
         * instance). This fallback applies only at this top level: an
         * object WITH an explicit chain of its own is not implicitly
         * rooted at `objectPrototype` unless its own construction put it
         * there (e.g. via `addParent`).
         *
         * A mutable receiver is resolved to its current snapshot first
         * (`getPrototype` does not do this), so the answer reflects the
         * object's current version's chain.
         *
         * Non-object receivers (SmallInteger, strings, lists, ...) are
         * answered through their prototype: `x.isInstanceOf(ctx, p)` is
         * `x.getPrototype(ctx) == p || <p is an ancestor of
         * x.getPrototype(ctx)>`. (The universal-root fallback above does
         * NOT additionally apply here beyond what `getPrototype` itself
         * already resolves to for the receiver's embedded type.)
         *
         * `hasParent` answers a closely related question with a narrower
         * interface (`int`, and `target == this` is also true, and it has
         * no universal-root fallback of its own — see its doc comment) —
         * pick whichever return convention the call site wants.
         */
        const ProtoObject* isInstanceOf(ProtoContext* context, const ProtoObject* prototype) const;

        //- Execution
        const ProtoObject* call(ProtoContext* context,
                                const ParentLink* nextParent,
                                const ProtoString* method,
                                const ProtoObject* self,
                                const ProtoList* positionalParameters,
                                const ProtoSparseList* keywordParametersDict = nullptr) const;
        
        const ProtoObject* divmod(ProtoContext* context, const ProtoObject* other) const;

        //- Internals & Type Checking
        unsigned long getHash(ProtoContext* context) const;
        int isCell(ProtoContext* context) const;
        const Cell* asCell(ProtoContext* context) const;
        static bool isCellPointer(const ProtoObject* obj);
        static const Cell* asCellPointer(const ProtoObject* obj);

        bool isBoolean(ProtoContext* context) const;
        bool isInteger(ProtoContext* context) const;
        bool isFloat(ProtoContext* context) const;
        /**
         * @brief Is the receiver a SmallInteger whose value fits in one byte?
         *
         * True for an EMBEDDED_VALUE pointer whose embedded type is SMALLINT
         * and whose value lies in the closed range [-128, 255]. False for
         * everything else.
         *
         * protoCore has no distinct byte type — there is no
         * POINTER_TAG_BYTE and no EMBEDDED_TYPE_BYTE,
         * `ProtoContext::fromByte(char)` is `fromInteger(char)`, and
         * `asByte` reads the low 8 bits of a SmallInteger. This predicate
         * therefore answers "would this value survive the byte round trip",
         * and the range is exactly the set that does: every `char` that
         * `fromByte` can encode (signed, -128..127) plus the unsigned
         * 0..255 reading that byte buffers and `asByte`'s callers use. So
         * `isByte(fromByte(c))` is true for every `char c`.
         *
         * False for integers outside that range and for LargeIntegers, and
         * false for every other kind of value: booleans and unicode chars
         * (each a distinct embedded type, NOT byte-valued integers),
         * PROTO_NONE, strings and symbols, byte buffers (`isByteBuffer` is
         * the unrelated buffer predicate), doubles, methods and objects.
         *
         * Like its neighbours this is a tag-only test: it allocates nothing,
         * dispatches nothing, and is safe on a null receiver.
         */
        bool isByte(ProtoContext* context) const;
        bool isDate(ProtoContext* context) const;
        bool isTimestamp(ProtoContext* context) const;
        bool isTimeDelta(ProtoContext* context) const;
        // P6 — tag-only fast paths.  These inline a single 6-bit tag-low
        // bitwise check for the dominant case where the receiver is a
        // direct primitive (tag 6 = string, 22 = symbol, 1 = embedded
        // value such as inline string / small int).  When the receiver
        // is a wrapper object (tag 0) that delegates type identity via
        // `__data__`, the slow virtual variant is invoked instead.
        // Hot callers (interpreter dispatchers, getAttribute auto-intern
        // probes, hash dispatch in protoPython / protoJS) avoid the
        // function-call overhead entirely on the common path.
        bool isMethod(ProtoContext* context) const;
        bool isNone(ProtoContext* context) const;
        bool isString(ProtoContext* context) const;
        // Tag-only fast variant: returns true if the pointer encodes a
        // string-typed value directly (POINTER_TAG_STRING = 6,
        // POINTER_TAG_SYMBOL = 22, or EMBEDDED_VALUE with
        // EMBEDDED_TYPE_INLINE_STRING = 4 in bits 6..9).  Returns false
        // for wrapper objects (tag 0); callers that need the full
        // protocol must call isString().
        static inline bool isStringTagFast(const ProtoObject* o) noexcept {
            if (!o) return false;
            uintptr_t p = reinterpret_cast<uintptr_t>(o);
            unsigned tag = static_cast<unsigned>(p & 0x3F);
            if (tag == 6 || tag == 22) return true;
            if (tag == 1) {
                unsigned emb = static_cast<unsigned>((p >> 6) & 0xF);
                if (emb == 4) return true;
            }
            return false;
        }
        bool isDouble(ProtoContext* context) const;
        bool isTuple(ProtoContext* context) const;
        bool isSet(ProtoContext* context) const;
        bool isMultiset(ProtoContext* context) const;
        bool isByteBuffer(ProtoContext* context) const;
        bool isNativeRangeIterator(ProtoContext* context) const;
        bool isMap(ProtoContext* context) const;

        //- Type Coercion
        bool asBoolean(ProtoContext* context) const;
        long long asLong(ProtoContext* context) const;
        /**
         * @brief Bignum-safe sign of an integer object.
         *
         * Returns -1 for negative integers, 0 for zero, +1 for positive.
         * Works for both SmallInteger (tagged) and LargeInteger (heap-allocated)
         * objects. Throws std::runtime_error if the receiver is not an integer.
         *
         * Public replacement for the previously private proto::Integer::sign.
         */
        int integerSign(ProtoContext* context) const;
        /**
         * @brief Bignum-safe integer-to-string conversion.
         *
         * Returns a ProtoString containing the receiver's integer value
         * rendered in the given base (2..36). Works for SmallInteger and
         * LargeInteger objects. Throws std::invalid_argument for an
         * out-of-range base, std::runtime_error if the receiver is not
         * an integer.
         *
         * Public replacement for the previously private
         * proto::Integer::toString.
         */
        const ProtoString* asIntegerString(ProtoContext* context, int base = 10) const;
        double asDouble(ProtoContext* context) const;
        char asByte(ProtoContext* context) const;
        void asDate(ProtoContext* context, unsigned int& year, unsigned& month, unsigned& day) const;
        unsigned long asTimestamp(ProtoContext* context) const;
        long asTimeDelta(ProtoContext* context) const;
        const ProtoList* asList(ProtoContext* context) const;
        const ProtoListIterator* asListIterator(ProtoContext* context) const;
        const ProtoTuple* asTuple(ProtoContext* context) const;
        const ProtoTupleIterator* asTupleIterator(ProtoContext* context) const;
        const ProtoString* asString(ProtoContext* context) const;
        const ProtoStringIterator* asStringIterator(ProtoContext* context) const;
        const ProtoSparseList* asSparseList(ProtoContext* context) const;
        const ProtoSparseListIterator* asSparseListIterator(ProtoContext* context) const;
        const ProtoMap* asMap(ProtoContext* context) const;
        const ProtoSet* asSet(ProtoContext* context) const;
        const ProtoSetIterator* asSetIterator(ProtoContext* context) const;
        const ProtoMultiset* asMultiset(ProtoContext* context) const;
        const ProtoMultisetIterator* asMultisetIterator(ProtoContext* context) const;
        const ProtoThread* asThread(ProtoContext* context) const;
        const ProtoExternalPointer* asExternalPointer(ProtoContext* context) const;
        const ProtoExternalBuffer* asExternalBuffer(ProtoContext* context) const;
        const ProtoByteBuffer* asByteBuffer(ProtoContext* context) const;
        const ProtoObject* nextInNativeRange(ProtoContext* context) const;
        /**
         * If this object is a ByteBuffer, returns its raw data pointer; otherwise nullptr.
         * Avoids three separate cross-DSO calls (isByteBuffer + asByteBuffer + getBuffer)
         * in hot paths such as FunctionMetaCache and native bytecode access.
         */
        char* getDataIfByteBuffer(ProtoContext* context) const;
        /** If this object is a ProtoExternalBuffer, returns the raw segment pointer; otherwise nullptr. Stable until the object is collected (no compaction). */
        void* getRawPointerIfExternalBuffer(ProtoContext* context) const;
        ProtoMethod asMethod(ProtoContext* context) const;
        const ProtoObject* asMethodSelf(ProtoContext* context) const;

        //- Comparison
        /**
         * @brief Three-way comparison for ordering: returns < 0, 0 or > 0.
         *
         * Numbers (SmallInteger, LargeInteger, double) compare by exact value
         * across kinds (-0.0 equals 0.0), strings by content, and any other
         * pair by address.  A NaN compares as 0 with every number, so this is
         * not an order for NaN; language comparison operators should use
         * partialCompare().
         */
        int compare(ProtoContext* context, const ProtoObject* other) const;

        /**
         * @brief IEEE partial-order comparison, for comparison operators.
         *
         * Numbers compare by exact value across SmallInteger, LargeInteger
         * and double; -0.0 is equivalent to 0.0 and to SmallInteger 0.  Any
         * NaN is unordered with everything, itself included: for a result `r`,
         * `r < 0`, `r <= 0`, `r == 0`, `r >= 0` and `r > 0` are all false and
         * `r != 0` is true, which is IEEE (and Python / JavaScript) semantics
         * for `< <= == >= > !=`.  Strings compare by content.  Any other pair
         * is equivalent when identical and unordered otherwise.
         * An embedder that wants identity-first equality (a NaN object equal
         * to itself) tests `a == b` first.
         */
        std::partial_ordering partialCompare(ProtoContext* context, const ProtoObject* other) const;

        //- Unary Operations
        const ProtoObject* negate(ProtoContext* context) const;
        const ProtoObject* abs(ProtoContext* context) const;

        //- Arithmetic Operations
        const ProtoObject* add(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* subtract(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* multiply(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* divide(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* modulo(ProtoContext* context, const ProtoObject* other) const;

        //- Bitwise Operations
        const ProtoObject* bitwiseAnd(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* bitwiseOr(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* bitwiseXor(ProtoContext* context, const ProtoObject* other) const;
        const ProtoObject* bitwiseNot(ProtoContext* context) const;
        const ProtoObject* shiftLeft(ProtoContext* context, int amount) const;
        const ProtoObject* shiftRight(ProtoContext* context, int amount) const;
    };

    // ------------------------------------------------------------------
    // Public SmallInt fast-path helpers
    // ------------------------------------------------------------------
    //
    // SmallInt tagged-pointer encoding (the same `si` bit-field layout
    // proto_internal.h declares for `ProtoObjectPointer`):
    //
    //   bits  0-5   :  pointer_tag  = 1  (POINTER_TAG_EMBEDDED_VALUE)
    //   bits  6-9   :  embedded_type = 0 (EMBEDDED_TYPE_SMALLINT)
    //   bits 10-63  :  signed 54-bit integer value (sign-extended on read)
    //
    // The combined low-10-bit tag is therefore the constant value 1.
    //
    // These helpers are `inline` in the public header so an embedder
    // (e.g. protoPython's bytecode dispatcher) can branch on the tag,
    // do the integer ALU op, and re-pack the result without crossing
    // the protoCore shared-library boundary.  protoCore stays a separate
    // shared library — only the bit pattern is exposed, not internal
    // types or symbols.
    //
    // Range is [-(2^53), (2^53) - 1]; values outside that fall through
    // to the slow path (`ProtoObject::add`, etc.) which promotes to a
    // LargeInteger.

    static constexpr long long PROTO_SMALL_INT_MAX  = (1LL << 53) - 1;
    static constexpr long long PROTO_SMALL_INT_MIN  = -(1LL << 53);
    static constexpr unsigned long PROTO_SMALL_INT_TAG_MASK  = 0x3FFUL; // pointer_tag(6) + embedded_type(4)
    static constexpr unsigned long PROTO_SMALL_INT_TAG_VALUE = 0x001UL; // POINTER_TAG_EMBEDDED_VALUE | (EMBEDDED_TYPE_SMALLINT << 6)

    /** True iff `obj` is a tagged SmallInteger pointer (no Cell, no allocation). */
    static inline bool isSmallInt(const ProtoObject* obj) {
        return (reinterpret_cast<unsigned long>(obj) & PROTO_SMALL_INT_TAG_MASK) == PROTO_SMALL_INT_TAG_VALUE;
    }

    /** Extract the signed 54-bit integer value from a SmallInt-tagged pointer.
     *  Caller must have validated with isSmallInt(). */
    static inline long long asSmallInt(const ProtoObject* obj) {
        // Arithmetic right shift on a signed 64-bit int sign-extends bit 63
        // into the upper 10 bits, recovering the original 54-bit signed value.
        return static_cast<long long>(reinterpret_cast<long long>(obj)) >> 10;
    }

    /** True iff `v` fits in a SmallInt (range [-(2^53), (2^53)-1]). */
    static inline bool smallIntInRange(long long v) {
        return v >= PROTO_SMALL_INT_MIN && v <= PROTO_SMALL_INT_MAX;
    }

    /** Build a SmallInt-tagged pointer from a value already known to be in range. */
    static inline const ProtoObject* makeSmallInt(long long v) {
        return reinterpret_cast<const ProtoObject*>((v << 10) | static_cast<long long>(PROTO_SMALL_INT_TAG_VALUE));
    }

    class ProtoListIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context) const;
        const ProtoListIterator* advance(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoList
    {
    public:
        //- Accessors
        const ProtoObject* getAt(ProtoContext* context, int index) const;
        const ProtoObject* getFirst(ProtoContext* context) const;
        const ProtoObject* getLast(ProtoContext* context) const;
        const ProtoList* getSlice(ProtoContext* context, int from, int to) const;
        unsigned long getSize(ProtoContext* context) const;
        bool has(ProtoContext* context, const ProtoObject* value) const;

        //- Modifiers that return a new list
        const ProtoList* setAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoList* insertAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoList* appendFirst(ProtoContext* context, const ProtoObject* value) const;
        const ProtoList* appendLast(ProtoContext* context, const ProtoObject* value) const;
        const ProtoList* extend(ProtoContext* context, const ProtoList* other) const;
        const ProtoList* splitFirst(ProtoContext* context, int index) const;
        const ProtoList* splitLast(ProtoContext* context, int index) const;
        const ProtoList* removeFirst(ProtoContext* context) const;
        const ProtoList* removeLast(ProtoContext* context) const;
        const ProtoList* removeAt(ProtoContext* context, int index) const;
        const ProtoList* removeSlice(ProtoContext* context, int from, int to) const;
        const ProtoList* multiply(ProtoContext* context, const ProtoObject* count) const;

        //- Conversion
        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoListIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoTupleIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context);
        const ProtoTupleIterator* advance(ProtoContext* context);
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoTuple
    {
    public:
        //- Accessors
        const ProtoObject* getAt(ProtoContext* context, int index) const;
        const ProtoObject* getFirst(ProtoContext* context) const;
        const ProtoObject* getLast(ProtoContext* context) const;
        const ProtoObject* getSlice(ProtoContext* context, int from, int to) const;
        unsigned long getSize(ProtoContext* context) const;
        bool has(ProtoContext* context, const ProtoObject* value) const;

        //- "Modifiers" (return new tuples)
        const ProtoObject* setAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoObject* insertAt(ProtoContext* context, int index, const ProtoObject* value) const;
        const ProtoObject* appendFirst(ProtoContext* context, const ProtoTuple* otherTuple) const;
        const ProtoObject* appendLast(ProtoContext* context, const ProtoTuple* otherTuple) const;
        const ProtoObject* splitFirst(ProtoContext* context, int count) const;
        const ProtoObject* splitLast(ProtoContext* context, int count) const;
        const ProtoObject* removeFirst(ProtoContext* context, int count) const;
        const ProtoObject* removeLast(ProtoContext* context, int count) const;
        const ProtoObject* removeAt(ProtoContext* context, int index) const;
        const ProtoObject* removeSlice(ProtoContext* context, int from, int to) const;

        //- Conversion
        const ProtoList* asList(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoTupleIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoStringIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context);
        const ProtoStringIterator* advance(ProtoContext* context);
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoString
    {
    public:
        static const ProtoString* create(ProtoContext* context, const ProtoList* list);

        // @deprecated Use fromUTF8() instead
        [[deprecated("Use fromUTF8 instead")]]
        static const ProtoString* fromUTF8String(ProtoContext* context,
                                                  const char* zeroTerminatedUtf8String);

        /**
         * Creates a ProtoString from a zero-terminated UTF-8 C string.
         *
         * Cost: one pass over the bytes, then one bottom-up build of the rope.
         * B bytes of text cost exactly `2 * ceil(B / 32)` cells — one 32-byte
         * leaf plus one internal node per 32 bytes, about 4 bytes of heap per
         * ASCII character — and leave no garbage behind. Up to 6 bytes of pure
         * ASCII cost no cell at all: the text lives in the tagged pointer.
         *
         * Malformed UTF-8 is tolerated rather than rejected: the bytes are
         * decoded to code points and re-encoded, so a truncated sequence
         * degrades to its lead byte and an overlong sequence collapses to its
         * shortest form. The result therefore does not always hold the
         * caller's exact bytes.
         *
         * Reads up to the first NUL. When the source is a buffer, prefer
         * fromUTF8Buffer: it takes an explicit length, can carry a NUL, and is
         * the only constructor that handles a multi-byte sequence split across
         * two reads.
         */
        static const ProtoString* fromUTF8(ProtoContext* context, const char* zeroTerminatedUtf8);

        /**
         * Creates a ProtoString from a std::string (UTF-8 encoded).
         *
         * Same cost and same malformed-input handling as fromUTF8, which it
         * delegates to. Note that it reads up to the first NUL, so a
         * std::string carrying an embedded NUL is truncated there; use
         * fromUTF8Buffer to build from the whole buffer.
         */
        static const ProtoString* fromStdString(ProtoContext* context, const std::string& s);

        /**
         * Decodes a chunk of raw UTF-8 bytes, handling incomplete multi-byte sequences
         * that span buffer boundaries.
         *
         * @param context       The current execution context.
         * @param buf           Pointer to the incoming byte buffer.
         * @param len           Number of bytes in \a buf.
         * @param pending       Bytes saved from the previous call that form the start of an
         *                      incomplete sequence, or nullptr if none.
         * @param pending_count Number of bytes in \a pending (0..3).
         * @param out_remainder Output buffer (minimum 4 bytes) that receives any trailing
         *                      incomplete sequence from \a buf that was not yet decoded.
         * @param out_remainder_count Number of bytes written to \a out_remainder.
         * @return The decoded ProtoString for the complete codepoints in this chunk.
         *
         * This is the entry point to prefer whenever the source is a buffer: it
         * takes a pointer and a length instead of scanning for a NUL, costs the
         * same `2 * ceil(B / 32)` cells with no garbage, and is the only
         * constructor that can carry an incomplete multi-byte sequence across a
         * buffer boundary. Reading a file in chunks through this and joining
         * them with appendLast — an O(log n) join that copies neither side — is
         * the cheapest way to build a large string.
         */
        static const ProtoString* fromUTF8Buffer(ProtoContext* context,
                                                  const uint8_t* buf, size_t len,
                                                  const uint8_t* pending,
                                                  uint8_t pending_count,
                                                  uint8_t* out_remainder,
                                                  uint8_t* out_remainder_count);

        /** Builds a ProtoString from a ProtoTuple of Unicode codepoint objects. */
        static const ProtoString* fromCodepointTuple(ProtoContext* context,
                                                      const ProtoTuple* tuple);

        /** Creates or retrieves an interned symbol for the given UTF-8 string. Symbols with the same
         *  content share a unique pointer identity. Strong symbols (created here) are never GC-collected. */
        static const ProtoString* createSymbol(ProtoContext* context, const char* zeroTerminatedUtf8);
        static const ProtoString* createSymbol(ProtoContext* context, const std::string& s);

        int cmp_to_string(ProtoContext* context, const ProtoString* otherString) const;

        /**
         * @brief True iff this ProtoString* points at an interned canonical
         * symbol (POINTER_TAG_SYMBOL).
         *
         * Strings created via createSymbol(), or auto-interned by
         * ProtoObject::setAttribute, are symbols.  Strings produced by
         * fromUTF8String / appendLast / setAt etc. are not.
         *
         * Equality and lookup against symbols is just pointer comparison
         * — embedders that handle attribute-name-style keys can short-
         * circuit content comparison when both sides are symbols.
         */
        bool isSymbol() const;

        //- Accessors
        const ProtoObject* getAt(ProtoContext* context, int index) const;
        unsigned long getSize(ProtoContext* context) const;
        const ProtoString* getSlice(ProtoContext* context, int from, int to) const;

        //- "Modifiers" (return new strings)
        const ProtoString* setAt(ProtoContext* context, int index, const ProtoObject* character) const;
        const ProtoString* insertAt(ProtoContext* context, int index, const ProtoObject* character) const;
        const ProtoString* setAtString(ProtoContext* context, int index, const ProtoString* otherString) const;
        const ProtoString* insertAtString(ProtoContext* context, int index, const ProtoString* otherString) const;
        const ProtoString* appendFirst(ProtoContext* context, const ProtoString* otherString) const;
        const ProtoString* appendLast(ProtoContext* context, const ProtoString* otherString) const;
        const ProtoString* splitFirst(ProtoContext* context, int count) const;
        const ProtoString* splitLast(ProtoContext* context, int count) const;
        const ProtoString* removeFirst(ProtoContext* context, int count) const;
        const ProtoString* removeLast(ProtoContext* context, int count) const;
        const ProtoString* removeAt(ProtoContext* context, int index) const;
        const ProtoString* removeSlice(ProtoContext* context, int from, int to) const;
        const ProtoString* multiply(ProtoContext* context, const ProtoObject* count) const;
        const ProtoObject* modulo(ProtoContext* context, const ProtoObject* other) const;

        //- Conversion
        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoList* asList(ProtoContext* context) const;
        const ProtoStringIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
        const ProtoString* isCell(ProtoContext* context) const;
        const Cell* asCell(ProtoContext* context) const;

        /** Appends the UTF-8 representation of this string to \a out. */
        void toUTF8String(ProtoContext* context, std::string& out) const;

        /** Returns the UTF-8 content of this string as a std::string. */
        std::string toStdString(ProtoContext* context) const;
    };

    //! ProtoExternalPointer - Represents a wrapper for external C++ pointers
    class ProtoExternalPointer
    {
    public:
        /**
         * @brief Extracts the wrapped C++ pointer from this ProtoExternalPointer.
         * @param context The current execution context.
         * @return The wrapped void* pointer, or nullptr if invalid.
         */
        void* getPointer(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    /** Contiguous buffer (aligned_alloc). Lifecycle tied to descriptor; GC finalize frees segment (Shadow GC). */
    class ProtoExternalBuffer
    {
    public:
        void* getRawPointer(ProtoContext* context) const;
        unsigned long getSize(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    /** Abstract base for module providers. Resolution chain entries "provider:alias" or "provider:GUID" delegate to a registered provider. */
    class ModuleProvider
    {
    public:
        virtual ~ModuleProvider() = default;
        /** Attempt to load a module for \a logicalPath. Return the module object or PROTO_NONE. */
        virtual const ProtoObject* tryLoad(const std::string& logicalPath, ProtoContext* ctx) = 0;
        /** Obligatory unique identifier. */
        virtual const std::string& getGUID() const = 0;
        /** Optional alias (e.g. "odoo_db"). Lookup by alias takes precedence over GUID. */
        virtual const std::string& getAlias() const = 0;
    };

    /** Singleton registry of ModuleProviders. Lookup by alias takes precedence over GUID. */
    class ProviderRegistry
    {
    public:
        static ProviderRegistry& instance();
        ~ProviderRegistry();
        void registerProvider(std::unique_ptr<ModuleProvider> provider);
        ModuleProvider* findByAlias(const std::string& alias);
        ModuleProvider* findByGUID(const std::string& guid);
        /** Given "provider:alias" or "provider:GUID", return the provider (alias tried first). Returns nullptr if not found or format invalid. */
        ModuleProvider* getProviderForSpec(const std::string& spec);
        ProviderRegistry(const ProviderRegistry&) = delete;
        ProviderRegistry& operator=(const ProviderRegistry&) = delete;
    private:
        ProviderRegistry();
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    class ProtoSparseListIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        unsigned long nextKey(ProtoContext* context) const;
        const ProtoObject* nextValue(ProtoContext* context) const;
        const ProtoSparseListIterator* advance(ProtoContext* context);
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoSparseList
    {
    public:
        bool has(ProtoContext* context, unsigned long index) const;
        const ProtoObject* getAt(ProtoContext* context, unsigned long index) const;
        const ProtoSparseList* setAt(ProtoContext* context, unsigned long index, const ProtoObject* value) const;
        const ProtoSparseList* removeAt(ProtoContext* context, unsigned long index) const;
        bool isEqual(ProtoContext* context, const ProtoSparseList* otherDict) const;
        unsigned long getSize(ProtoContext* context) const;

        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoSparseListIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;

        void processElements(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, unsigned long, const ProtoObject*)) const;
        void processValues(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const ProtoObject*)) const;
    };

    /**
     * @class ProtoMapIterator
     * @brief Ascending key-word iteration over a ProtoMap version.
     *
     * Maintainer option D1 = (a): this handle is an unboxed C++ pointer, not a
     * ProtoObject word. It offers no `asObject`/object-model API and is never
     * given to the attribute chain.
     */
    class ProtoMapIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* nextKey(ProtoContext* context) const;
        const ProtoObject* nextValue(ProtoContext* context) const;
        const ProtoMapIterator* advance(ProtoContext* context) const;
    };

    /**
     * @class ProtoMap
     * @brief Persistent map from `const ProtoObject*` keys to values.
     *
     * Identical to ProtoSparseList except that the key is an object word the
     * garbage collector traces: an object whose only reference is a key stays
     * alive.  Keys are ordered and compared by their word (identity, tag
     * included); an embedded value (SmallInteger, boolean, char, None) is a
     * valid key and is never traced.  nullptr is not a valid key.  getAt
     * returns nullptr for an absent key, so a stored PROTO_NONE stays
     * distinguishable.  Every modifier returns a new version; old versions
     * remain valid.  As with ProtoSparseList, setAt of a key to its current
     * value may still allocate a new version (callers must not rely on
     * receiving the same pointer back), and getIterator returns nullptr for
     * an empty map.
     */
    class ProtoMap
    {
    public:
        bool has(ProtoContext* context, const ProtoObject* key) const;
        const ProtoObject* getAt(ProtoContext* context, const ProtoObject* key) const;
        const ProtoMap* setAt(ProtoContext* context, const ProtoObject* key, const ProtoObject* value) const;
        const ProtoMap* removeAt(ProtoContext* context, const ProtoObject* key) const;
        bool isEqual(ProtoContext* context, const ProtoMap* other) const;
        unsigned long getSize(ProtoContext* context) const;

        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoMapIterator* getIterator(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;

        /** Visits (key, value) in ascending key-word order.  Allocates nothing and
         *  holds no GC critical section, so `method` may allocate and run
         *  arbitrary code.  The caller keeps the collection reachable. */
        void processElements(ProtoContext* context, void* self,
            void (*method)(ProtoContext*, void*, const ProtoObject* key, const ProtoObject* value)) const;
        void processValues(ProtoContext* context, void* self,
            void (*method)(ProtoContext*, void*, const ProtoObject* value)) const;
    };

    /**
     * @brief A language's key semantics for the hashed-collection helper.
     *
     * isIdentityKey: true when the language's equality for this key is
     * identity.  hash / equals: the language's hash and equality, used only for
     * value-equality keys (equals only inside a collision bucket).
     * SmallInteger keys always take the value-equality path, so hash and
     * equals are also called for them regardless of what isIdentityKey would
     * answer (isIdentityKey is not consulted for SmallInteger keys).  Callbacks
     * run outside any GC critical section and may allocate.
     */
    struct KeySemantics {
        bool (*isIdentityKey)(ProtoContext*, const ProtoObject* key);
        unsigned long (*hash)(ProtoContext*, const ProtoObject* key);
        bool (*equals)(ProtoContext*, const ProtoObject* a, const ProtoObject* b);
    };

    /**
     * Persistent hashed map operations over a ProtoMap
     * (protoScala/docs/platform/PROTOMAP-SPEC.md §4):
     *  - identity key       -> slot key = the key itself, slot value = v
     *  - value-equality key -> slot key = SmallInteger word of the hash's low
     *                          54 bits, slot value = flat ProtoList
     *                          [k0, v0, k1, v1, ...] (one pair unless the hash
     *                          collides)
     * SmallInteger keys always take the value-equality path, so the two slot
     * kinds can never collide.  Putting an existing equal key keeps the stored
     * key and replaces its value.
     *
     * A nullptr key is ignored silently by every function (D3): hashedPut and
     * hashedRemove return `map`, hashedGet returns nullptr, and no callback
     * runs.  A nullptr value in hashedPut removes the key, as
     * ProtoMap::setAt does.
     *
     * A map used with these functions must be read and modified only through
     * them (with one KeySemantics): mixing them with raw setAt / removeAt on
     * the same map is undefined, because a raw SmallInteger key would collide
     * with a hash slot.
     */
    const ProtoMap* hashedPut(ProtoContext* context, const ProtoMap* map,
                                           const KeySemantics& semantics, const ProtoObject* key, const ProtoObject* value);
    const ProtoObject* hashedGet(ProtoContext* context, const ProtoMap* map,
                                 const KeySemantics& semantics, const ProtoObject* key);
    const ProtoMap* hashedRemove(ProtoContext* context, const ProtoMap* map,
                                              const KeySemantics& semantics, const ProtoObject* key);
    void hashedForEach(ProtoContext* context, const ProtoMap* map, void* self,
                       void (*fn)(ProtoContext*, void*, const ProtoObject* key, const ProtoObject* value));

    class ProtoSetIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context) const;
        const ProtoSetIterator* advance(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        //! The hash the element next() returns is stored under (see ProtoSet::addWithHash).
        unsigned long nextHash(ProtoContext* context) const;
    };

    /**
     * @class ProtoSet
     * @brief An immutable collection of unique objects.
     */
    class ProtoSet
    {
    public:
        /**
         * @brief Returns a new set with the given value added.
         */
        const ProtoSet* add(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns PROTO_TRUE if the value is in the set, otherwise PROTO_FALSE.
         */
        const ProtoObject* has(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns a new set with the given value removed.
         */
        const ProtoSet* remove(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns a new set with `value` stored under `hash`, replacing any element with that hash.
         * @details add/has/remove key elements by ProtoObject::getHash. A language whose equality
         * differs (Python makes 1, 1.0 and True one element, and honours __hash__) supplies its own
         * hash through these variants; a set must then be accessed with one hash function only.
         */
        const ProtoSet* addWithHash(ProtoContext* context, unsigned long hash, const ProtoObject* value) const;

        /**
         * @brief Returns true if an element is stored under `hash`.
         */
        bool hasHash(ProtoContext* context, unsigned long hash) const;

        /**
         * @brief Returns a new set without the element stored under `hash` (the same set if absent).
         */
        const ProtoSet* removeHash(ProtoContext* context, unsigned long hash) const;

        /**
         * @brief Returns the number of unique elements in the set.
         */
        unsigned long getSize(ProtoContext* context) const;

        /**
         * @brief Returns the set as a generic ProtoObject.
         */
        const ProtoObject* asObject(ProtoContext* context) const;

        /**
         * @brief Returns an iterator for the set.
         */
        const ProtoSetIterator* getIterator(ProtoContext* context) const;
    };

    class ProtoMultisetIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context) const;
        const ProtoMultisetIterator* advance(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    /**
     * @class ProtoMultiset
     * @brief An immutable collection that can store multiple occurrences of the same object.
     */
    class ProtoMultiset
    {
    public:
        /**
         * @brief Returns a new multiset with the given value added.
         */
        const ProtoMultiset* add(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns the number of times the given value appears in the multiset.
         */
        const ProtoObject* count(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns a new multiset with one occurrence of the given value removed.
         */
        const ProtoMultiset* remove(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns the total number of elements in the multiset (including duplicates).
         */
        unsigned long getSize(ProtoContext* context) const;

        /**
         * @brief Returns the multiset as a generic ProtoObject.
         */
        const ProtoObject* asObject(ProtoContext* context) const;

        /**
         * @brief Returns an iterator for the multiset.
         */
        const ProtoMultisetIterator* getIterator(ProtoContext* context) const;
    };

    class ProtoByteBuffer
    {
    public:
        unsigned long getSize(ProtoContext* context) const;
        char* getBuffer(ProtoContext* context) const;
        char getAt(ProtoContext* context, int index) const;
        void setAt(ProtoContext* context, int index, char value);
        const ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;
    };

    class ProtoThread
    {
    public:
        static const ProtoThread* getCurrentThread(ProtoContext* context);

        void detach(ProtoContext* context);
        void join(ProtoContext* context);
        void exit(ProtoContext* context);

        const ProtoObject* getName(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        unsigned long getHash(ProtoContext* context) const;

        void setCurrentContext(ProtoContext* context);
        /** Returns the current execution context for this thread. O(1) thread-local read. */
        ProtoContext* getCurrentContext() const;
        void synchToGC();

        /**
         * @brief Mark the calling thread as ABOUT to enter unmanaged
         *        code (typically a blocking OS call: `read`, `write`,
         *        `sleep`, `poll`, network I/O, etc.).
         *
         * While the unmanaged counter is > 0, this thread does NOT
         * participate in the stop-the-world quorum — the GC may run a
         * collection without waiting for the thread to reach a
         * safepoint. The thread is treated as "permanently parked" for
         * the duration of the unmanaged region.
         *
         * The contract while unmanaged:
         *   * The thread MUST NOT touch any `ProtoObject*` value
         *     (read, write, allocate, dispatch). The GC may run
         *     concurrently and move / reclaim cells; any pointer the
         *     thread held becomes UB.
         *   * Every call MUST be paired with a matching
         *     `returnFromUnmanaged()`. Calls nest: a re-entrant
         *     blocking call inside another unmanaged region is fine;
         *     only the outermost pair changes the thread's GC
         *     participation.
         *
         * After incrementing the counter, this also notifies the GC —
         * if it was waiting for this very thread to reach the quorum,
         * the wait can complete immediately.
         *
         * Pair with `returnFromUnmanaged()`; in C++ prefer
         * `ProtoContext::UnmanagedScope` (RAII).
         */
        void goUnmanaged();

        /**
         * @brief Mark the calling thread as RETURNING from unmanaged
         *        code. Matches an earlier `goUnmanaged()`.
         *
         * Decrements the unmanaged counter; on the outermost pair
         * (counter back to 0) the thread re-joins the GC quorum. If
         * the GC is currently in a stop-the-world phase, this call
         * BLOCKS until the STW phase clears — exactly like a normal
         * safepoint park — so the returning thread cannot resume
         * touching `ProtoObject*` while a collection is still
         * scanning roots.
         */
        void returnFromUnmanaged();
    };

    /**
     * @brief Represents the execution context for a thread, managing the call stack,
     * local variables, and object creation.
     */
    class ProtoContext
    {
    private:
        // C-style array for automatic variables that are destroyed with the context.
        const ProtoObject** automaticLocals;
        unsigned int automaticLocalsCount;
        // False when automaticLocals points to an externally-owned buffer (e.g. stack SBO).
        // The destructor only calls delete[] when this is true.
        bool ownsSlots_;
        // True only for the collector's own allocation context (see
        // GCOwnedTag).  Such a context never registers as a thread's
        // current context or as ProtoSpace::mainContext, so its destructor
        // must not unregister it either.
        bool gcOwned_ = false;

    public:
        /**
         * @brief Constructs a new execution context for a function call.
         * This constructor is the core of the function execution model. It allocates
         * space for local variables and performs argument-to-parameter binding.
         *
         * @param space The global ProtoSpace this context belongs to.
         * @param previous The parent context in the call stack.
         * @param parameterNames A list of ProtoStrings for the function's declared parameter names.
         * @param localNames A list of ProtoStrings for the function's automatic (C-style) local variables.
         * @param args The positional arguments passed to the function.
         * @param kwargs The keyword arguments passed to the function.
         * @param totalSlots Minimum number of slots to allocate for local variables.
         * @param externalSlots Optional pre-allocated slot buffer (caller-owned, must stay alive
         *        for the lifetime of this context). When non-null, no heap allocation is done for
         *        slots; the caller is responsible for initialising the buffer to PROTO_NONE.
         */
        explicit ProtoContext(
            ProtoSpace* space,
            ProtoContext* previous = nullptr,
            const ProtoList* parameterNames = nullptr,
            const ProtoList* localNames = nullptr,
            const ProtoList* args = nullptr,
            const ProtoSparseList* kwargs = nullptr,
            size_t totalSlots = 0,
            const ProtoObject** externalSlots = nullptr
        );

        /** @brief Selects the constructor of the collector's own allocation context. */
        struct GCOwnedTag {};

        /**
         * @brief Constructs the garbage collector's own allocation context.
         *
         * Used once per ProtoSpace, for `ProtoSpace::gcContext`; embedders
         * never need it.  The context has no thread, no previous context and
         * no local slots, so it allocates from its own freelist under its
         * spinlock rather than from any thread's freelist.  Unlike every
         * other constructor path it does NOT register itself as
         * `space->mainContext` and it is not part of any thread's context
         * chain, so the stop-the-world root scan never sees it.  Only the GC
         * thread allocates through it, during a cycle, and the collector
         * submits its young generation before the cycle ends.
         */
        ProtoContext(GCOwnedTag, ProtoSpace* space);

        ~ProtoContext();

        //- Execution State
        ProtoContext* previous;
        ProtoSpace* space;
        ProtoThread* thread;
        char* currentFileName;
        int currentLineNumber;
        
        // Variables that can be captured by closures, managed by the GC.
        const ProtoSparseList* closureLocals;

        //- Return Value
        const ProtoObject* returnValue;

        //- GC Interface
        /**
         * @brief Provides the GC with access to the automatic local variables.
         * @return A pointer to the array of automatic local variable objects.
         */
        inline const ProtoObject** getAutomaticLocals() const { return automaticLocals; }

        /**
         * @brief Provides the GC with the count of automatic local variables.
         * @return The number of automatic local variables.
         */
        inline unsigned int getAutomaticLocalsCount() const { return automaticLocalsCount; }

        /**
         * @brief Read one automatic-local slot (combined bounds-check + read).
         *
         * Preferred over getAutomaticLocals()[idx] at call sites: single
         * expression, bounds-safe, and keeps the slot access within the
         * protoCore API boundary.  Returns nullptr when idx is out of range.
         */
        inline const ProtoObject* getAutomaticLocal(unsigned int idx) const {
            return (idx < automaticLocalsCount && automaticLocals) ? automaticLocals[idx] : nullptr;
        }

        /**
         * @brief Write one automatic-local slot (combined bounds-check + write).
         *
         * Returns true on success, false if idx is out of range.
         */
        inline bool setAutomaticLocal(unsigned int idx, const ProtoObject* val) {
            if (idx < automaticLocalsCount && automaticLocals) {
                automaticLocals[idx] = val;
                return true;
            }
            return false;
        }

        /**
         * @brief Resize the automatic-locals slot region.
         *
         * Designed for embedders (e.g. protoJS's bytecode interpreter)
         * that don't know the required slot count at construction time
         * but want a flat, GC-visible array — instead of the more
         * expensive ProtoSparseList copy-on-write storage — for the
         * call frame's locals + value stack.
         *
         * If `newCount > automaticLocalsCount`, allocates a fresh
         * heap-owned `const ProtoObject*[newCount]` initialised to
         * PROTO_NONE, copies any existing entries, frees the previous
         * heap buffer (only when `ownsSlots_`), and reseats the
         * pointer.  After the call `automaticLocals` is guaranteed to
         * be heap-owned (`ownsSlots_=true`) so the destructor will
         * release it.
         *
         * If `newCount <= automaticLocalsCount`, the call is a no-op
         * (we never shrink — keeps the existing GC roots reachable).
         */
        void resizeAutomaticLocals(unsigned int newCount);

        //- Factory methods for primitive types.
        const ProtoObject* fromInteger(long long value);
        const ProtoObject* fromLong(long long value);
        const ProtoObject* fromString(const char* str, int base = 10);
        const ProtoObject* fromDouble(double value);
        const ProtoObject* fromUnicodeChar(unsigned int unicodeChar);
        const ProtoObject* fromUTF8String(const char* zeroTerminatedUtf8String);
        const ProtoObject* fromMethod(ProtoObject* self, ProtoMethod method);
        /**
         * @brief Wraps an opaque C++ pointer in a collectable object.
         *
         * `finalizer`, when given, is called with `pointer` once the object
         * is collected.  It runs on the GC thread during sweep, concurrently
         * with the application threads, and follows the finalizer contract:
         * it only completes an action on an external structure (release a
         * resource, update a counter).  It must not allocate protoCore
         * objects, call protoCore APIs, dereference other ProtoObject*, or
         * block.  See docs/GarbageCollector.md § "Finalizer contract".
         */
        const ProtoObject* fromExternalPointer(void* pointer, void (*finalizer)(void*) = nullptr);
        const ProtoObject* fromBuffer(unsigned long length, char* buffer, bool freeOnExit = false);
        const ProtoObject* newBuffer(unsigned long length);
        const ProtoObject* fromBoolean(bool value);
        const ProtoObject* fromByte(char c);
        const ProtoObject* fromDate(unsigned year, unsigned month, unsigned day);
        const ProtoObject* fromTimestamp(unsigned long timestamp);
        const ProtoObject* fromTimeDelta(long timedelta);

        //- Factory methods for complex types
        // Empty list — returns the inline-storage form (POINTER_TAG_LIST_SMALL,
        // size 0).  Subsequent appendLast calls stay in the inline form for
        // sizes 1..5 and only graduate to the AVL form on overflow.
        const ProtoList* newList();
        // Bulk-construct a list from a count + contiguous source array in
        // a SINGLE cell allocation when n ≤ 5; for n > 5 it falls back to
        // the AVL builder.  Use this whenever the count is known at the call
        // site (interpreter call dispatch, native method argument packs);
        // the result is functionally identical to newList() + N appendLast
        // but costs exactly 1 cell instead of 1 + N.
        const ProtoList* newList(unsigned n, const ProtoObject* const* items);
        const ProtoTuple* newTuple();
        const ProtoTuple* newTuple(const std::vector<const ProtoObject*>& elements);
        const ProtoTuple* newTupleFromList(const ProtoList* sourceList);
        const ProtoSparseList* newSparseList();
        /** Empty ProtoMap (inline Small form; promotes past 3 entries). */
        const ProtoMap* newMap();
        // Returns an empty AVL-form sparse list implementation as a raw
        // C++ pointer. Used for internal struct fields that should not
        // carry a tag (e.g. ProtoObjectCell::attributes); the public
        // newSparseList() above returns a Small-form public-API handle
        // for callers that want the inline-3 optimisation (closures,
        // Set/Multiset backing).
        const ProtoSparseListImplementation* newSparseListImpl();
        const ProtoSet* newSet();
        /** Creates a native range iterator over [start, stop) with the given step. */
        const ProtoObject* newRangeIterator(long long start, long long stop, long long step);
        const ProtoMultiset* newMultiset();
        const ProtoObject* newObject(bool mutableObject = false);
        /** Allocates a contiguous buffer (aligned_alloc). GC finalize frees it when descriptor is collected (Shadow GC). */
        const ProtoObject* newExternalBuffer(unsigned long size);
        /**
         * Create a fresh, GC-owned ProtoByteBuffer holding `len` raw octets.
         * The bytes are copied from `data` (data may be null only if len == 0).
         * Unlike ProtoString, ProtoByteBuffer is opaque-binary: every byte
         * 0..255 round-trips, and embedded nulls do not truncate.
         */
        const ProtoByteBuffer* newByteBuffer(const char* data, unsigned long len);

        //- Memory Management
        Cell* allocCell();
        void addCell2Context(Cell* cell);

        /**
         * @brief Cooperative GC safepoint.
         *
         * Public hook callable from any thread.  When the space's stop-the-world
         * flag is set, the calling thread parks itself on the STW condition
         * variable until the GC pause ends, the same handshake `allocCell`
         * already performs internally every 64 allocations.
         *
         * This is the supported way for an embedder (e.g. protoPython's
         * bytecode dispatch loop) to participate in the GC handshake from a
         * tight, allocation-free hot loop.  Without it, a CPU-bound thread
         * that never calls `allocCell` will starve the GC indefinitely and
         * stall every other thread waiting for STW to begin.
         *
         * Cheap on the fast path: a single relaxed atomic load of
         * `stwFlag`.  Only takes the global mutex if the flag is set.
         */
        void safepoint();

        /**
         * @brief Enter an unmanaged region on this context's thread.
         *
         * Delegates to `thread->goUnmanaged()`. Call this immediately
         * before a blocking OS call (read / write / sleep / poll /
         * accept / network I/O / any syscall that may not return for
         * arbitrary time) and pair with `returnFromUnmanaged()` after
         * the call returns. While unmanaged, the GC may run a
         * stop-the-world phase WITHOUT waiting for this thread — the
         * thread is treated as "permanently parked" for quorum
         * purposes.
         *
         * Invariants while unmanaged:
         *   * NO `ProtoObject*` access (the GC may move / reclaim cells).
         *   * No allocation through this context.
         *   * No `setAttribute` / `getAttribute` / dispatch.
         *
         * Calls nest. Prefer the RAII helper `UnmanagedScope` below to
         * guarantee pairing under exceptions.
         *
         * No-op when this context has no thread (early bootstrap).
         */
        void goUnmanaged();

        /**
         * @brief Leave the unmanaged region. Mirror of `goUnmanaged()`.
         *
         * Decrements the thread's unmanaged counter; on the outermost
         * pair (counter back to 0) the thread re-enters the GC
         * quorum. If a stop-the-world phase is in progress at that
         * moment, this BLOCKS until it clears — the returning thread
         * is treated as a normal safepoint park while the GC finishes.
         *
         * Failing to call this after a matching `goUnmanaged()` keeps
         * the thread out of the GC quorum permanently, but does not
         * corrupt anything; the worst case is that the GC's quorum
         * count drifts off the real running-thread count.
         *
         * No-op when this context has no thread.
         */
        void returnFromUnmanaged();

        /**
         * @brief RAII helper for an unmanaged region.
         *
         * Usage:
         * @code
         *   {
         *       ProtoContext::UnmanagedScope u(ctx);
         *       ssize_t n = ::read(fd, buf, sz);  // OS call; GC may run
         *   }   // dtor calls returnFromUnmanaged automatically
         * @endcode
         *
         * Guarantees the matching `returnFromUnmanaged()` runs on
         * every exit path (normal return, exception, early break).
         * Holds a pointer to the context — must not outlive it.
         */
        class UnmanagedScope {
        public:
            explicit UnmanagedScope(ProtoContext* c) : ctx_(c) {
                if (ctx_) ctx_->goUnmanaged();
            }
            ~UnmanagedScope() {
                if (ctx_) ctx_->returnFromUnmanaged();
            }
            UnmanagedScope(const UnmanagedScope&) = delete;
            UnmanagedScope& operator=(const UnmanagedScope&) = delete;
            UnmanagedScope(UnmanagedScope&& o) noexcept : ctx_(o.ctx_) { o.ctx_ = nullptr; }
            UnmanagedScope& operator=(UnmanagedScope&&) = delete;
        private:
            ProtoContext* ctx_;
        };

        /**
         * @brief Heap-ceiling checkpoint, run at every outermost critical
         *        section boundary (see CriticalSection).
         *
         * When a hard heap limit is configured (ProtoSpace::setHeapLimits)
         * and the heap has reached its ceiling, this blocks the calling
         * thread until the GC reclaims enough cells to proceed — or, if the
         * live set itself meets the ceiling across two cycles, escalates to
         * the out-of-memory handling.  It is a no-op when no limit is set.
         *
         * It MUST run at criticalSectionDepth == 0: the thread holds no
         * half-built tree there, so it can leave the running set and let the
         * GC run.  Blocking inside a critical section would instead let a
         * stop-the-world cycle reclaim a helper's in-flight, not-yet-anchored
         * cells.  See ProtoSpace::waitForHeapHeadroom.
         */
        void heapLimitCheckpoint();

        Cell* lastAllocatedCell;
        unsigned long allocatedCellsCount;
        Cell* freeCells;

        // Cached pointer to this thread's MutableValueCacheEntry array
        // (stashed at context construction to short-circuit the
        // `toImpl(thread) → extension → mutableValueCache` indirection
        // chain that resolveMutableState would otherwise pay on every
        // mutable read).  See protoCore/core/ProtoObject.cpp's
        // resolveMutableState for the validation/refresh discipline.
        // nullptr when the thread has no extension yet (e.g. early
        // bootstrap, off-thread alloc with NULL context).
        MutableValueCacheEntry* mutableValueCache_;
        Cell* pendingRoot;
        std::atomic_flag lock{ATOMIC_FLAG_INIT};

        /**
         * @brief Depth counter for in-progress GC critical sections on this
         *        thread (e.g. mutable setAttribute mid-construction).
         *
         * While > 0, the cooperative STW poll in allocCell()/safepoint() skips
         * parking — the GC's stop-the-world phase therefore cannot start
         * until every running thread either finishes its critical section
         * or reaches a safepoint outside one.  This protects the construct +
         * CAS-into-root pattern from being interrupted between the
         * allocation of intermediate cells and the final atomic publish:
         * without the guard a sweep would see those cells in dirtySegments
         * (chain submitted at the per-context threshold) and unreachable
         * from any GC root, and free them under the running mutator.
         *
         * Only mutated by the owning thread; never touched concurrently.
         * Wrap any tree-builder that allocates ≥1 cell *and* attaches them
         * to a GC root only via a final CAS in a CriticalSection RAII guard.
         */
        unsigned int criticalSectionDepth = 0;

        /**
         * @brief RAII guard for a GC critical section on a ProtoContext.
         *
         * Increments criticalSectionDepth on construction and decrements on
         * destruction; STW polling in allocCell()/safepoint() skips parking
         * while the depth is non-zero.  Use this around any code that holds
         * ProtoObject* / Cell* values in C++ locals across one or more
         * allocations and later attaches them to a GC root (typically by
         * CAS'ing a new mutable snapshot into a mutableRoot shard).  The
         * guard is per-thread, no atomics, no lock.
         */
        class CriticalSection {
        public:
            explicit CriticalSection(ProtoContext* ctx) : ctx_(ctx) {
                if (ctx_) {
                    // Enforce the heap ceiling only at the outermost section:
                    // here criticalSectionDepth is still 0, so the thread
                    // holds no half-built tree and may safely block for the
                    // GC.  Nested sections (depth > 0) skip the check — a
                    // helper is mid-construction with un-anchored cells.
                    if (ctx_->criticalSectionDepth == 0)
                        ctx_->heapLimitCheckpoint();
                    ctx_->criticalSectionDepth++;
                }
            }
            ~CriticalSection() {
                if (ctx_) ctx_->criticalSectionDepth--;
            }
            CriticalSection(const CriticalSection&) = delete;
            CriticalSection& operator=(const CriticalSection&) = delete;
        private:
            ProtoContext* ctx_;
        };
        
        ProtoContext(const ProtoContext&) = delete;
        ProtoContext& operator=(const ProtoContext&) = delete;
    };

    /**
     * @brief The main container for the Proto runtime environment.
     *
     * A ProtoSpace manages the global state, including the object heap,
     * garbage collector, and all running threads. It holds the root const prototypes* for all built-in types.
     */
    /**
     * @brief A registry of GC roots owned by an embedder (e.g. a JS or
     *        Python runtime built on protoCore).
     *
     * Embedders frequently need to keep a `ProtoObject` alive across an
     * allocation boundary that is invisible to protoCore's tracing GC —
     * the canonical case is an asynchronous callback whose receiver is
     * captured into a C++ lambda that fires from a non-protoCore event
     * loop.  Without an explicit root the GC may reclaim the object
     * between enqueue and dispatch.
     *
     * `ProtoRootSet` solves this without forcing every embedder to
     * invent its own anchor scheme on top of `setAttribute`-on-globals
     * (which serialises through the mutable shard locks and is prone to
     * silent CAS livelock under contention).  Each embedder asks the
     * `ProtoSpace` for one or more root sets, calls `add()` to pin a
     * `ProtoObject*` (receiving an opaque `Handle`), and `remove()` to
     * release it.  The GC's marking phase iterates every registered
     * root set during STW and treats their contents as additional
     * roots — see `ProtoSpace::forEachRootSet`.
     *
     * Thread-safe: `add` / `remove` / `resolve` are safe to call from
     * any thread.  `forEach` is intended to be called only from the GC
     * thread during STW; mutators are parked at that point so no
     * concurrent `add`/`remove` can race with iteration.
     */
    class ProtoRootSet
    {
    public:
        using Handle = unsigned long long;
        static constexpr Handle kNullHandle = 0;

        /**
         * @brief Pin `obj` as a GC root.  Returns an opaque handle that
         *        the caller must later pass to `remove()`.  Pinning a
         *        null pointer is a no-op and returns `kNullHandle`.
         */
        Handle add(const ProtoObject* obj);

        /**
         * @brief Look up the object behind a handle without removing it.
         *        Returns nullptr if the handle is unknown or already
         *        removed.  Convenient when the lambda owns the handle
         *        and wants to dispatch on the pinned value.
         */
        const ProtoObject* resolve(Handle h) const;

        /**
         * @brief Release the root pinned by `add`.  Safe to call with
         *        `kNullHandle` (no-op).  Each handle should be removed
         *        exactly once.
         */
        void remove(Handle h);

        /** @brief Number of currently pinned roots — for tests/diagnostics. */
        unsigned long size() const;

        /** @brief Human-readable name set at creation, for diagnostics. */
        const char* getName() const;

        /**
         * @brief Iterate every currently-pinned root.  The visitor is
         *        invoked once per live slot.  Holds the internal mutex
         *        for the duration so it is safe to call concurrently
         *        with `add` / `remove`, but the GC normally calls this
         *        during STW where the lock is uncontended.
         */
        void forEachRoot(void (*visit)(void* user, const ProtoObject* obj),
                          void* user) const;

    private:
        friend class ProtoSpace;
        ProtoRootSet(ProtoSpace* owner, const char* name);
        ~ProtoRootSet();

        struct Impl;
        Impl* impl_;
    };

    class ProtoSpace
    {
    public:
        explicit ProtoSpace();
        ~ProtoSpace();

        //- Core Prototypes
        ProtoObject* objectPrototype{};
        ProtoObject* smallIntegerPrototype{};
        ProtoObject* largeIntegerPrototype{};
        ProtoObject* floatPrototype{};
        ProtoObject* unicodeCharPrototype{};
        ProtoObject* bytePrototype{};
        ProtoObject* nonePrototype{};
        ProtoObject* methodPrototype{};
        ProtoObject* bufferPrototype{};
        ProtoObject* pointerPrototype{};
        ProtoObject* booleanPrototype{};
        ProtoObject* doublePrototype{};
        ProtoObject* datePrototype{};
        ProtoObject* timestampPrototype{};
        ProtoObject* timedeltaPrototype{};
        ProtoObject* threadPrototype{};
        ProtoObject* rootObject{};

        //- Collection Prototypes
        ProtoObject* listPrototype{};
        ProtoObject* listIteratorPrototype{};
        ProtoObject* tuplePrototype{};
        ProtoObject* tupleIteratorPrototype{};
        ProtoObject* stringPrototype{};
        ProtoObject* stringIteratorPrototype{};
        ProtoObject* sparseListPrototype{};
        ProtoObject* sparseListIteratorPrototype{};
        ProtoObject* setPrototype{};
        ProtoObject* setIteratorPrototype{};
        ProtoObject* multisetPrototype{};
        ProtoObject* multisetIteratorPrototype{};
        ProtoObject* rangeIteratorPrototype{};
        ProtoObject* mapPrototype{};

        // --- Cached Literals ---
        ProtoString* literalData;
        ProtoString* literalSetAttribute;
        ProtoString* literalCallMethod;

        //- Callbacks
        ProtoObject* (*nonMethodCallback)(
            ProtoContext* context,
            const ParentLink* nextParent,
            const ProtoString* method,
            const ProtoObject* self,
            const ProtoList* unnamedParametersList,
            const ProtoSparseList* keywordParametersDict){};

        ProtoObject* (*attributeNotFoundGetCallback)(
            ProtoContext* context,
            const ProtoObject* self,
            const ProtoString* attributeName){};

        ProtoObject* (*parameterNotFoundCallback)(
            ProtoContext* context,
            const ProtoObject* self,
            const ProtoString* attributeName){};

        ProtoObject* (*parameterTwiceAssignedCallback)(
            ProtoContext* context,
            const ProtoObject* self,
            const ProtoString* attributeName){};


        ProtoObject* (*outOfMemoryCallback)(
            ProtoContext* context){};

        ProtoObject* (*invalidConversionCallback)(
            ProtoContext* context){};

        //- Public Methods
        /**
         * @brief Submits a chain of newly created cells (a "young generation")
         * from a returning context to the garbage collector.
         * @param cellChain A pointer to the first cell in the linked list.
         */
        void submitYoungGeneration(const Cell* cellChain);
        
        void deallocMutable(unsigned long mutable_ref);
        const ProtoList* getThreads(ProtoContext* context) const;
        const ProtoThread* newThread(
            ProtoContext *c,
            const ProtoString* name,
            ProtoMethod mainFunction,
            const ProtoList* args,
            const ProtoSparseList* kwargs);

        //- Memory Management & GC
        Cell* getFreeCells(ProtoContext* ctx);
        void analyzeUsedCells(Cell* cellsChain);
        void triggerGC();

        /**
         * @brief Configure the heap allocation watermarks (both in Cells).
         *
         * `softCells` — above this, the Cell allocator prefers GC reclamation
         * over growing the heap (it waits one GC cycle before requesting more
         * memory from the OS).  `hardCells` — the hard ceiling: `heapSize`
         * never exceeds it for ordinary mutator allocations; a thread that
         * would cross it waits for the GC and, if the live working set itself
         * meets the ceiling, the configured out-of-memory path runs.
         *
         * Passing `0` for a limit disables it.  Both `0` (the default) gives
         * unbounded allocation — behaviour identical to a build with no limit.
         * `softCells` is clamped to `<= hardCells` when both are non-zero.
         *
         * The GC thread bypasses the ceiling (it cannot wait on itself).
         * The limit is enforced at critical-section boundaries (see
         * ProtoContext::heapLimitCheckpoint); an allocation that exhausts the
         * cell pool *inside* a critical section may overshoot by at most one
         * OS batch, since it cannot block there.
         */
        void setHeapLimits(int softCells, int hardCells);

        /**
         * @brief Block until the heap has room to satisfy an allocation, or
         *        escalate to out-of-memory handling.
         *
         * Called from ProtoContext::heapLimitCheckpoint (a critical-section
         * boundary) and from the depth-0 path of getFreeCells.  When the heap
         * is at its ceiling with no free cells, it leaves the running set,
         * waits for the GC to reclaim, and re-checks.  If two consecutive GC
         * cycles confirm the live set itself meets the ceiling, it invokes
         * `outOfMemoryCallback` once and then performs a controlled abort.
         *
         * A no-op when no hard limit is configured, on the GC thread, or for
         * a contextless caller.  The caller MUST NOT hold globalMutex and
         * MUST be at criticalSectionDepth == 0.
         */
        void waitForHeapHeadroom(ProtoContext* ctx);

        //- Embedder Root Sets
        //
        // Lets a runtime built on protoCore (e.g. protoJS, protoPython)
        // pin ProtoObjects as GC roots without smuggling them into
        // setAttribute-on-the-global hacks.  See `ProtoRootSet` for the
        // full motivation and threading model.
        //
        // `name` is copied for diagnostics.  Caller must `destroyRootSet`
        // the returned handle before the ProtoSpace is destroyed (or
        // leak on shutdown — the destructor frees any still-registered
        // sets).
        ProtoRootSet* createRootSet(const char* name);
        void destroyRootSet(ProtoRootSet* rs);

        /**
         * @brief Invokes `visit(user, rs)` for every currently-registered
         *        root set.  Called from the GC thread during STW; embedders
         *        should not call this themselves.
         */
        void forEachRootSet(void (*visit)(void* user, ProtoRootSet* rs),
                             void* user) const;

        //- Thread Management
        void allocThread(ProtoContext* context, const ProtoThread* thread);
        void deallocThread(ProtoContext* context, const ProtoThread* thread);
        const ProtoList* getThreads(ProtoContext* context);
        const ProtoThread* getCurrentThread(ProtoContext* context);
        const ProtoThread* getThreadByName(ProtoContext* context, const ProtoString* threadName);

        /** Returns the current resolution chain (ProtoList of ProtoString). Default is platform-dependent if not set. */
        const ProtoObject* getResolutionChain() const;
        /** Sets the resolution chain. \a newChain must be a ProtoList of ProtoString; if null or invalid, restores default chain. */
        void setResolutionChain(const ProtoObject* newChain);
        /** Resolve and load a module by \a logicalPath using this space's resolution chain. Returns a wrapper object with attribute \a attrName2create pointing to the module, or PROTO_NONE. Thread-safe; uses SharedModuleCache. */
        const ProtoObject* getImportModule(ProtoContext* context, const char* logicalPath, const char* attrName2create);

        /**
         * @brief Creates and starts a new managed thread within this ProtoSpace.
         * @param context The current ProtoContext from which the thread is being created.
         * @param threadName A ProtoString representing the name of the new thread.
         * @param target The ProtoMethod to be executed by the new thread.
         * @param args A ProtoList of positional arguments for the target method.
         * @param kwargs A ProtoSparseList of keyword arguments for the target method.
         * @return A pointer to the newly created ProtoThread object.
         */

        const ProtoSpaceImplementation* impl{};
        int state;
        ProtoContext* rootContext;
        // 256 independent shards: mutable_ref % MUTABLE_ROOT_SHARDS selects the shard.
        // Reduces CAS contention and AVL depth in multi-threaded workloads; transparent to API callers.
        // Each shard slot is padded to 64 bytes; the array is cache-line aligned so consecutive
        // slots fall on distinct cache lines, eliminating false sharing between cores
        // performing CAS on different shards.
        static constexpr int MUTABLE_ROOT_SHARDS = 256;
        struct MutableShardSlot {
            std::atomic<ProtoSparseList*> root;
            char _pad[64 - sizeof(std::atomic<ProtoSparseList*>)];
        };
        alignas(64) MutableShardSlot mutableRoot[MUTABLE_ROOT_SHARDS];

        /**
         * @brief Per-cycle snapshot of mutableRoot, captured atomically at STW.
         *
         * Consumed by the concurrent mark phase.  Workers continue to CAS
         * mutableRoot post-STW; the snapshot is unaffected.  The snapshot is
         * the formal mark-time dereference table: any GC code path that needs
         * to resolve a mutable_ref to its STW-time current value during mark
         * MUST read this table, never mutableRoot[].root.load().
         *
         * Lifecycle: captured at STW Phase 2 (gcThreadLoop).  Cleared at the
         * end of the GC cycle.  Outside that window all entries are nullptr.
         *
         * Size: MUTABLE_ROOT_SHARDS pointers (256 * 8 = 2 KB).  Independent
         * of heap size or live-object count.  Cache-friendly: fits in 32
         * cache lines.
         *
         * Why no atomics: written only by the GC thread under STW (workers
         * parked), read only by the GC thread during mark (no other thread
         * reads it).  Plain pointers are sufficient.
         *
         * See docs/GarbageCollector.md § "Concurrent Mark Without Barriers"
         * and docs/STW_ELIMINATION_RESEARCH.md § 13 for the rationale of
         * snapshot-replaces-barrier in protoCore's concentrated-mutability
         * design.
         */
        ProtoSparseList* gcMutableSnapshot[MUTABLE_ROOT_SHARDS];

        std::atomic<unsigned long> nextMutableRef;

        // --- Maquinaria Interna (Público por ahora) ---

        ProtoSparseList* threads;
        Cell* freeCells;
        // Tail pointer for the global freeCells linked list.  Maintained
        // alongside `freeCells` so getFreeCells can take the entire list in
        // O(1) when the requested batchSize swallows everything available
        // (the common case during warmup and after a small sweep). When
        // freeCells is null, freeCellsTail is also null.
        Cell* freeCellsTail;

        /**
         * @brief Pre-chunked freelist (path #5 v2 chunked GC).
         *
         * Sweep accumulates dead cells into chunks of CELL_CHUNK_SIZE while
         * walking segments (negligible extra cost; we already touch every
         * cell).  At chunk boundary the chunk is published to `freeChunks`.
         *
         * `getFreeCells` pops one chunk in O(1) (no linked-list walk to find
         * a cut point) and returns its head as the thread's refill batch.
         * The chunk struct itself is recycled to `freeChunkPool` after use.
         *
         * `freeCells` remains for the partial / OS-fallback paths; chunks are
         * the fast path.  Both are protected by the same `globalMutex`.
         */
        struct FreeChunk {
            Cell* head;
            Cell* tail;
            unsigned long count;
            FreeChunk* next;
        };
        FreeChunk* freeChunks;
        FreeChunk* freeChunkPool;
        static constexpr unsigned long CELL_CHUNK_SIZE = 8192;

        /** Lock-free stack of dirty segments; GC drains under globalMutex. */
        std::atomic<DirtySegment*> dirtySegments;
        /**
         * Lock-free free pool of unused DirtySegments.  submitYoungGeneration
         * pops from here before falling back to a fresh allocation; the GC
         * pushes consumed segments back here after sweep instead of freeing
         * them.  Eliminates the per-Python-method-call malloc/free pair on
         * the hot path (~480 K/run on richards_lite).
         */
        std::atomic<DirtySegment*> dirtySegmentFreePool;

        /**
         * @brief Holding pen for sweep survivors when stagger > 1.
         *
         * The survivor re-chain (PROTOCORE_GC_REINCLUDE_SURVIVORS) re-pushes
         * cells that were marked alive into a follow-up segment so the next
         * cycle can re-examine them.  By default that "next cycle" is
         * literally the next one, which means every live cell pays the
         * mark cost once per cycle.  With stagger > 1, sweep instead pushes
         * survivors here; the pen is folded back into `dirtySegments` only
         * once every `survivorStagger` cycles, so survivors are re-examined
         * less often.  RSS grows by up to `stagger × working set` because
         * cells that became unreachable during the staggered window stay
         * alive in the pen until the next fold; mark cost across the same
         * window drops to ~`1/stagger` of the un-staggered baseline.
         *
         * Lock-free LIFO push from sweep, atomic-exchange-to-nullptr drain
         * from gcThreadLoop's STW phase.  Always-empty when stagger == 1.
         */
        std::atomic<DirtySegment*> survivorPen;

        /**
         * @brief How many GC cycles between successive folds of the
         *        survivor pen back into dirtySegments.
         *
         * 1 = re-check every cycle (current behaviour, no stagger).
         * 2 = re-check every other cycle.
         * N = re-check every Nth cycle.
         *
         * Default: 1 (no stagger).  Override at startup via env var
         * PROTOCORE_GC_SURVIVOR_STAGGER.  Set once at ProtoSpace
         * construction, never modified afterwards.
         */
        static constexpr unsigned int SURVIVOR_STAGGER_DEFAULT = 1;
        unsigned int survivorStagger;

        /**
         * @brief Monotonic GC cycle counter.
         *
         * Incremented by the GC thread (single writer) at the start of each
         * cycle, inside STW.  Read by mutator threads via `getGCCycleCount()`
         * to invalidate per-thread caches that hold raw cell pointers
         * (e.g. embedder behavior caches keyed by ProtoObject*).
         *
         * Atomic with relaxed ordering: the value is only used as a
         * change-detection token, not as a synchronisation primitive.
         * Mutator reads observing a stale value will simply postpone
         * cache invalidation until the next call, which is safe because
         * the cache is consulted with the same pointer on the next hot
         * path entry.
         *
         * When `gcCycleCount % survivorStagger == 0`, the survivor pen is
         * folded back into `dirtySegments` before the cycle's root collection.
         */
        std::atomic<uint64_t> gcCycleCount;

        /**
         * @brief Cells found reachable by the most recently completed GC
         * cycle's mark phase.  Published by the GC thread at end of cycle;
         * used only for the diagnostic message of the out-of-memory abort.
         */
        std::atomic<unsigned long> liveCellsLastCycle;

        /**
         * @brief Cells reclaimed (swept into the freelist) by the most
         * recently completed GC cycle.  Published by the GC thread at end of
         * cycle.
         *
         * This is the authoritative out-of-memory signal for the heap-limit
         * path (ProtoSpace::waitForHeapHeadroom): once the heap is at its
         * ceiling, two consecutive completed cycles that each reclaim zero
         * cells mean no collection can free space — genuine, unrecoverable
         * OOM.  Reclamation, not mark-phase reachability, is the metric:
         * retained-but-unmarked cells (a live context's un-submitted young
         * generation) fill the heap without ever entering `markedList`, so a
         * mark-based count would under-report and miss that OOM.
         */
        std::atomic<unsigned long> reclaimedLastCycle;

        /**
         * @brief Notified by the GC thread at the end of every cycle, once the
         * sweep has published reclaimed Cells to the freelist.  A thread
         * parked in getFreeCells() waiting for memory wakes here and re-checks.
         */
        std::condition_variable_any memoryReclaimedCV;

        /**
         * @brief Returns the current GC cycle counter (relaxed load).
         *
         * Embedders cache cell-pointer-keyed data on the assumption that the
         * pointer remains valid; after a GC cycle, freed pointers can be
         * recycled, so caches must be invalidated.  Compare the returned
         * value against a thread-local snapshot; any change means at least
         * one GC cycle has happened since the last check, so caches must
         * be cleared before reuse.
         */
        uint64_t getGCCycleCount() const { return gcCycleCount.load(std::memory_order_relaxed); }

        /**
         * @brief Per-context allocation threshold for the GC trigger.
         *
         * When PROTOCORE_GC_REINCLUDE_SURVIVORS is enabled and a single
         * ProtoContext has allocated more than this many cells since its
         * last GC submission, allocCell() submits the context's young
         * chain to dirtySegments, resets the per-context count, and calls
         * triggerGC().  This bounds RSS in tight loops with small working
         * sets without changing the algorithm: the GC still marks from
         * roots and frees what is unreachable.
         *
         * Default: CONTEXT_GC_THRESHOLD_DEFAULT (10000 cells).
         * Override at startup via env var PROTOCORE_GC_CONTEXT_THRESHOLD.
         *
         * Read by ProtoContext::allocCell() in the hot path; not atomic
         * because it is set once at construction and never changed.
         */
        static constexpr unsigned int CONTEXT_GC_THRESHOLD_DEFAULT = 10000;
        unsigned int maxAllocatedCellsPerContext;
        int blocksPerAllocation;
        int heapSize;
        /**
         * @brief Hard heap ceiling, in Cells.  `heapSize` never exceeds this
         * for ordinary mutator allocations.  `0` disables the ceiling.
         * Set via setHeapLimits(); see getFreeCells() for the enforcement.
         */
        int maxHeapSize;
        /**
         * @brief Soft heap watermark, in Cells.  Above it, the Cell allocator
         * prefers GC reclamation over growing the heap.  `0` disables it.
         */
        int softHeapLimit;
        int freeCellsCount;
        unsigned int gcSleepMilliseconds;
        // Unused: the tuple interner is `tupleInterner`. Kept, like
        // stringInternMap, so the ProtoSpace layout does not change.
        std::atomic<TupleDictionary*> tupleRoot;
        void* stringInternMap;
        SymbolTable* symbolTable{};
        TupleInterner* tupleInterner{};
        std::atomic<bool> mutableLock;
        std::atomic<bool> threadsLock;
        std::atomic<bool> gcLock;
        std::thread::id mainThreadId;
        std::unique_ptr<std::thread> gcThread; // Usando unique_ptr
        std::condition_variable_any stopTheWorldCV;
        std::condition_variable_any restartTheWorldCV;
        std::condition_variable_any gcCV;
        std::atomic<bool> gcStarted;
        std::atomic<int> runningThreads;
        std::atomic<bool> stwFlag;
        std::atomic<int> parkedThreads;
        ProtoContext* mainContext;

        const ProtoList* resolutionChain_;
        std::vector<const ProtoObject*> moduleRoots;
        std::mutex moduleRootsMutex;

        /** @brief Global reentrant mutex for protecting space-wide metadata (interning, thread registry, GC state). */
        static std::recursive_mutex globalMutex;

        // --- Embedder root sets (see `createRootSet`) ---
        std::vector<ProtoRootSet*> rootSets_;
        mutable std::mutex rootSetsMutex_;

        /**
         * @brief The garbage collector's own allocation context.
         *
         * Built with ProtoContext::GCOwnedTag: no thread, its own freelist,
         * never registered as `mainContext` and never scanned as a root.
         * Only the GC thread allocates through it, and only inside a cycle,
         * after sweep; no stop-the-world can begin while it holds a
         * half-built structure, because the GC thread is the only thread
         * that starts one.  The collector submits its young generation
         * before the cycle ends, so everything allocated through it is a
         * candidate of the next cycle.
         */
        ProtoContext* gcContext{};

        /**
         * @brief `mutable_ref`s of the handles finalized by the current
         *        cycle's sweep.
         *
         * GC-thread collector bookkeeping of the same kind as the mark work
         * list and `markedList`: `ProtoObjectCell::finalize` appends a
         * collected handle's `mutable_ref`, and the release phase after sweep
         * removes those entries from `mutableRoot` and clears the list,
         * keeping its capacity.  A plain C++ vector of integers, tightly
         * scoped to one cycle on one thread: it holds no protoCore objects
         * and no mutator ever sees it.  See docs/GarbageCollector.md
         * § "Phase 5b".
         */
        std::vector<unsigned long> gcFinalizedMutableRefs;
    };
}

#endif /* PROTO_H_ */
