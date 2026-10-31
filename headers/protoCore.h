/*
 * protoCore
 *
 *  Created on: November, 2017 - Redesign January, 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */

#ifndef PROTO_H_
#define PROTO_H_

#include <atomic>
#include <cstdint>
#include <compare>
#include <condition_variable>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

// 64-bit integer types of the protoCore API.
//
// protoCore was written for LP64, where long is 64 bits.  Windows is LLP64 (long
// is 32 bits), so there the API uses long long.  Everywhere else these ARE long
// and unsigned long, so the types, the C++ mangling and the ABI on Linux and
// macOS are exactly what they were before Windows was supported.
#if defined(_WIN32)
#define PROTO_L(x)  x##LL
#define PROTO_UL(x) x##ULL
#define PROTO_FMT_U "llu"   // printf length+conversion for proto_ulong
#else
#define PROTO_L(x)  x##L
#define PROTO_UL(x) x##UL
#define PROTO_FMT_U "lu"
#endif

// ProtoSpace::setThreadStackBytes exists (protoCore 2.8.0 and later).
#define PROTOCORE_HAS_THREAD_STACK_BYTES 1
// ProtoSpace::currentThreadStackBytes exists, and setThreadStackBytes is
// honoured on Linux, macOS and Windows (protoCore 2.9.0 and later).
#define PROTOCORE_HAS_CURRENT_THREAD_STACK_BYTES 1
// ProtoSpace::liveSpaceCount exists (protoCore 3.1.0 and later).
#define PROTOCORE_HAS_LIVE_SPACE_COUNT 1

// Static data members of the API.  A Windows DLL exports functions to callers
// that know nothing about it, but data must be declared dllimport by the
// caller.  Empty everywhere else.
#if defined(_WIN32) && defined(PROTOCORE_BUILDING_DLL)
#define PROTOCORE_DATA __declspec(dllexport)
#elif defined(_WIN32)
#define PROTOCORE_DATA __declspec(dllimport)
#else
#define PROTOCORE_DATA
#endif

namespace proto
{
#if defined(_WIN32)
    using proto_long  = long long;
    using proto_ulong = unsigned long long;
#else
    using proto_long  = long;
    using proto_ulong = unsigned long;
#endif

    class SymbolTable;  // forward declaration: the process-wide lock-free symbol trie
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
    struct DirtySegment;
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
    class ProtoArguments;
    class ProtoKeywordArguments;
    class ProtoMap;
    class ProtoMapIterator;
    class ProtoMPSCQueue;
    class ProtoLinkedList;
    class ProtoLinkedListCursor;
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
    #define PROTO_TRUE ((const proto::ProtoObject*)  PROTO_UL(1217)) // Tag: EMBEDDED_VALUE (1), Type: BOOLEAN (3), Value: 1
    #define PROTO_FALSE ((const proto::ProtoObject*) PROTO_UL(193))  // Tag: EMBEDDED_VALUE (1), Type: BOOLEAN (3), Value: 0
    #define PROTO_NONE ((const proto::ProtoObject*)  PROTO_UL(321))  // Tag: EMBEDDED_VALUE (1), Type: NONE (5), Value: 0

    /**
     * @brief A method: the one definition every call form reaches.
     *
     * The arguments arrive as descriptors that the caller builds for the
     * duration of the call, either over arrays (the array form of
     * ProtoObject::call) or over a ProtoList and a ProtoSparseList (the list
     * form).  The method binds its parameters by reading them, the same way
     * whatever form the caller used: `positional->getAt(context, i)`,
     * `keywords->getAt(context, key)`.  Either pointer may be null when the
     * call has no such arguments.  A descriptor is valid only during the
     * call; a method that keeps its arguments keeps `asList(context)` (or
     * `asSparseList`), never the descriptor.
     *
     * See docs/CALLING_CONVENTION.md.
     */
    typedef const ProtoObject*(*ProtoMethod)(
        ProtoContext* context,
        const ProtoObject* self,
        const ParentLink* parentLink,
        const ProtoArguments* positionalParameters,
        const ProtoKeywordArguments* keywordParameters
    );

    /**
     * @brief The errors protoCore itself raises.
     *
     * protoCore never throws a C++ standard exception at an embedder: an
     * operation that fails raises a `ProtoException` whose value the space's
     * error factory built for one of these kinds (see
     * `ProtoSpace::setErrorFactory` and docs/EXCEPTIONS.md).
     */
    enum class ProtoErrorKind : int {
        //! A malformed argument: an unparsable integer literal, a base outside
        //! 2..36, a negative shift amount.
        InvalidArgument = 0,
        //! An operand of the wrong type: an integer operation on a value that
        //! is not an integer.
        TypeMismatch = 1,
        //! A result that does not fit the requested representation (a large
        //! integer read as a 64-bit value).
        Overflow = 2,
        //! Integer division or modulo by zero.
        DivisionByZero = 3,
        //! A call with more positional arguments than the method's parameters.
        ArityMismatch = 4,
    };

    //! Number of `ProtoErrorKind` values.
    constexpr int kProtoErrorKindCount = 5;

    /**
     * @brief Builds the value protoCore raises for an error of `kind`.
     *
     * A runtime installs one per space so that protoCore's errors arrive as
     * the language's own exceptions (a division by zero as Python's
     * `ZeroDivisionError`, Scala's `ArithmeticException`, ...).  It runs on
     * the raising thread, with `context` the raising context, and may
     * allocate.  It must return a value; it must not throw.
     */
    typedef const ProtoObject* (*ProtoErrorFactory)(
        ProtoContext* context, ProtoErrorKind kind, const char* message);

    /**
     * @brief The one C++ exception of every language on protoCore.
     *
     * A language-level `throw` / `raise` / `signal` that unwinds is a C++
     * `throw` of a `ProtoException` carrying the thrown value; a handler is a
     * C++ `catch (const proto::ProtoException&)`.  The exception's type is the
     * prototype chain of `value()`, never a C++ class: a handler selects by
     * asking `isInstanceOf(context, type)` (or its language's own test over
     * the same chain) and rethrows what it does not handle.  That is what lets
     * a module written in one language, or in C++, define an error type that
     * every other language catches by name.
     *
     * While the exception is in flight its value is pinned in a root set of
     * the value's space, so a collection during unwinding (a `finally`, a
     * destructor that allocates) cannot reclaim it.  Copies share the pin;
     * the last copy releases it.  An exception must not outlive its space.
     *
     * It deliberately does not derive from `std::exception`: a language value
     * is not a C++ failure, and a `catch (const std::exception&)` written for
     * library errors must not swallow it.  Runtimes may derive from it to
     * carry their own extras (a traceback, a source line); handlers still
     * catch the base.  The destructor is defined in libprotoCore, so the type
     * has one identity across every shared library of the process.
     *
     * See docs/EXCEPTIONS.md and docs/tutorials/exceptions/.
     */
    class ProtoException
    {
    public:
        /** @brief An exception carrying `value`, pinned in `context`'s space. */
        ProtoException(ProtoContext* context, const ProtoObject* value);
        virtual ~ProtoException();
        ProtoException(const ProtoException&) = default;
        ProtoException& operator=(const ProtoException&) = default;

        /** @brief The thrown value: a language exception object, or any value. */
        const ProtoObject* value() const noexcept { return value_; }

        /**
         * @brief Whether the value is an instance of `type`: `type` is on the
         *        value's prototype chain (ProtoObject::isInstanceOf).
         */
        bool isInstanceOf(ProtoContext* context, const ProtoObject* type) const;

    private:
        struct Pin;
        const ProtoObject* value_;
        std::shared_ptr<Pin> pin_;
    };

    /**
     * @brief One keyword argument of the array call form.
     *
     * `name` is an interned symbol (ProtoString::createSymbol); its key is
     * the symbol's pointer, as ProtoObject::setAttribute keys attributes.
     * Interned symbols are perennial, so the name needs no rooting.
     */
    struct ProtoKeywordArgument {
        const ProtoString* name;
        const ProtoObject* value;
    };

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

        //- Lazy objects (3.1.0)
        /**
         * @brief A new IMMUTABLE child of this object, as `newChild`, that
         * also carries a lazy slot: a value computed at most once, on
         * demand, by `producer`, and cached in the object.
         *
         * Why a new type.  A runtime that caches a computed value in a
         * MUTABLE object pays a write to the global mutable table for every
         * value.  Until 3.1.0 every state in that table was also a collector
         * root until the object's handle had been swept, so a lazy sequence
         * built that way was freed one link per cycle; since 3.1.0 a state
         * is live only while its handle is reached
         * (docs/GLOBAL_MUTABLE_TABLE.md, "States are not roots").  A value
         * computed once still needs no table entry and no shard
         * compare-and-swap.  A lazy object is an ordinary immutable object cell
         * with two more references, traced only through the object itself,
         * like any other field: an unreachable lazy object takes its value
         * with it in the very cycle that finds it dead.
         *
         * The slot moves one way:
         *
         *     Unrealized --claimLazy--> Claimed --publishLazy--> Realized
         *         |  ^------unclaimLazy-----'                      ^
         *         '------------------publishLazy-------------------'
         *
         * `claimLazy` lets exactly one caller run the producer (the others
         * wait for, or poll, the published value); `publishLazy` installs the
         * value once, whoever calls it, and answers false to every later
         * call. A caller that computes the value without claiming (a pure
         * producer, safe to run twice) publishes from Unrealized and, when it
         * loses, reads the winner's value.  Once Realized, the object no
         * longer keeps `producer` alive: the collector stops tracing it from
         * the first cycle that starts after the publication, and
         * `getLazyProducer` answers nullptr.  Every transition is a single
         * compare-and-set; nothing allocates, nothing locks.
         *
         * The object is immutable in every other respect: its prototype
         * chain is fixed and `setAttribute` answers a new, ordinary
         * (non-lazy) object, as for every immutable object.
         *
         * Called on a non-object (an embedded value, a list, ...), creates
         * the lazy child of that value's prototype, as `newChild` does.
         *
         * Several spaces: the value is an ordinary reference.  A value built
         * by a thread of another space is a cell of that space, and the
         * limitation of docs/GLOBAL_MUTABLE_TABLE.md on cells of one space
         * held only by another applies to it unchanged.
         */
        const ProtoObject* newLazyChild(ProtoContext* context, const ProtoObject* producer) const;

        //! The states of a lazy object's slot (`getLazyState`).
        enum LazyState : int {
            LazyNotLazy    = -1,  //!< not a lazy object
            LazyUnrealized = 0,   //!< the producer has not run (or raised)
            LazyClaimed    = 1,   //!< one caller is running the producer
            LazyPublishing = 2,   //!< a value is being installed (a few instructions)
            LazyRealized   = 3    //!< the value is published
        };
        //! True for an object made by newLazyChild.
        bool isLazyObject(ProtoContext* context) const;
        //! The state of the lazy slot; LazyNotLazy for any other value.
        int getLazyState(ProtoContext* context) const;
        //! The producer while the slot is Unrealized or Claimed; nullptr once
        //! a publication has begun (Publishing or Realized: the collector may
        //! free it from the next cycle on) and for a non-lazy value. A caller
        //! that uses it across an allocation or a safepoint roots it first.
        const ProtoObject* getLazyProducer(ProtoContext* context) const;
        //! The published value once Realized; nullptr before.
        const ProtoObject* getLazyValue(ProtoContext* context) const;
        //! Unrealized -> Claimed; true for the one caller that wins.
        bool claimLazy(ProtoContext* context) const;
        //! Claimed -> Unrealized: the producer raised, and a later claim may
        //! run it again. The claimer only.
        void unclaimLazy(ProtoContext* context) const;
        //! Installs `value` (nullptr is stored as PROTO_NONE) from Unrealized
        //! or Claimed; false, changing nothing, once a value is published or
        //! being published.
        bool publishLazy(ProtoContext* context, const ProtoObject* value) const;

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
         * @brief Writes a group of attributes as ONE new version (since 2.11.0).
         *
         * The result is exactly what `count` chained `setAttribute` calls
         * produce, in array order: `names[i] := values[i]`, a later entry for
         * the same name wins, a `nullptr` value removes the name, a `nullptr`
         * name is skipped, and heap-string names are interned as
         * `setAttribute` interns them.
         *
         * - **Immutable receiver**: answers the new immutable version; the
         *   receiver is untouched (identical to the chained calls).
         * - **Mutable receiver**: reads the current snapshot once, derives
         *   the new version from it, and publishes that version into the
         *   mutable table with ONE compare-and-swap on the shard root,
         *   answering `this`.  If another writer publishes first, the whole
         *   group is reapplied onto the newer snapshot and published again:
         *   every entry is "set to value", so a retry is always correct and
         *   writes by other threads to other names are never lost.  The
         *   group is atomic to other threads: a reader sees all of it or
         *   none of it, never a prefix.
         *
         * This is the immutable-style form of `o.f1 = a; o.f2 = b; ...`:
         * `tmp = current(o); tmp = tmp.f1 := a; tmp = tmp.f2 := b; publish(o,
         * tmp)`.  It changes no part of the object model; it only spares the
         * intermediate publications (one snapshot cell and one shard-root
         * path copy per name).
         *
         * `names` and `values` must each hold `count` entries; `count == 0`
         * is a no-op that answers `this` without allocating.  Values that
         * are reachable only from the caller's arrays must be reachable from
         * a live context (EMBEDDER-CONFORMANCE rule 3), as for `setAttribute`.
         */
        const ProtoObject* setAttributes(ProtoContext* context, unsigned count,
                                         const ProtoString* const* names,
                                         const ProtoObject* const* values) const;
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
         * where the caller guarantees name is canonical: a symbol or an inline string, as
         * ProtoString::createSymbol and ProtoString::intern return (e.g. co_names entries).
         * A plain heap string misses silently.
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
         * arbitrary embedder code.  What keeps the attribute tree
         * reachable while the callback runs is the caller's own root for
         * the receiver plus, for a MUTABLE receiver, a pin of the resolved
         * snapshot in a root set of the space for the duration of the walk:
         * a mutable object's superseded state is not reachable from its
         * handle (states are not roots, docs/GLOBAL_MUTABLE_TABLE.md), so a
         * write from the callback or from another thread followed by a
         * collection would otherwise free the tree being walked.  The pin
         * is a C++-side root-set entry, not a Cell, held in a root set of
         * the calling thread's own (see "Concurrent walks" below).
         *
         * Concurrent walks: any number of threads may walk, the same
         * receiver or different ones, at the same time.  A thread counted in
         * the space's stop-the-world quorum -- its main thread, or a
         * ProtoThread walking in the space that started it -- pins in a root
         * set only it writes, with no lock at all (the collector reads root
         * sets only while such threads are parked).  Any other thread pins in
         * one of 16 mutex-guarded sets, assigned round-robin at its first
         * walk; beyond 16 such threads, threads share a set, which costs
         * contention, never correctness.  An immutable receiver is not
         * pinned at all.
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
        /**
         * @brief Calls the method named `method` with the arguments in a
         *        ProtoList and a ProtoSparseList (the list form).
         *
         * The method receives descriptors over the two lists; nothing is
         * copied.  Either list may be null.  Looks `method` up in the
         * receiver's attribute chain; a value that is not a method goes to
         * the space's nonMethodCallback, or answers PROTO_NONE.
         */
        const ProtoObject* call(ProtoContext* context,
                                const ParentLink* nextParent,
                                const ProtoString* method,
                                const ProtoObject* self,
                                const ProtoList* positionalParameters,
                                const ProtoSparseList* keywordParametersDict = nullptr) const;
        /**
         * @brief Calls the method named `method` with the arguments in
         *        arrays (the array form).
         *
         * The same lookup and the same method as the list form; the method
         * receives descriptors over the caller's arrays, and nothing is
         * allocated.  The caller chooses where the arrays live and how large
         * they are, and keeps their values reachable for the duration of the
         * call; keyword names are interned symbols.  See
         * docs/CALLING_CONVENTION.md.
         */
        const ProtoObject* call(ProtoContext* context,
                                const ParentLink* nextParent,
                                const ProtoString* method,
                                const ProtoObject* self,
                                const ProtoObject* const* positional,
                                unsigned positionalCount,
                                const ProtoKeywordArgument* keywords = nullptr,
                                unsigned keywordCount = 0) const;
        /**
         * @brief Calls the method named `method` with descriptors the caller
         *        already holds, such as the ones a method received and passes
         *        on.  `keywords` may be null.
         */
        const ProtoObject* call(ProtoContext* context,
                                const ParentLink* nextParent,
                                const ProtoString* method,
                                const ProtoObject* self,
                                const ProtoArguments& positional,
                                const ProtoKeywordArguments* keywords = nullptr) const;
        
        const ProtoObject* divmod(ProtoContext* context, const ProtoObject* other) const;

        //- Internals & Type Checking
        proto_ulong getHash(ProtoContext* context) const;
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
        bool isMPSCQueue(ProtoContext* context) const;
        bool isLinkedList(ProtoContext* context) const;

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
        proto_ulong asTimestamp(ProtoContext* context) const;
        proto_long asTimeDelta(ProtoContext* context) const;
        const ProtoList* asList(ProtoContext* context) const;
        const ProtoListIterator* asListIterator(ProtoContext* context) const;
        const ProtoTuple* asTuple(ProtoContext* context) const;
        const ProtoTupleIterator* asTupleIterator(ProtoContext* context) const;
        const ProtoString* asString(ProtoContext* context) const;
        const ProtoStringIterator* asStringIterator(ProtoContext* context) const;
        const ProtoSparseList* asSparseList(ProtoContext* context) const;
        const ProtoSparseListIterator* asSparseListIterator(ProtoContext* context) const;
        const ProtoMap* asMap(ProtoContext* context) const;
        const ProtoMPSCQueue* asMPSCQueue(ProtoContext* context) const;
        const ProtoLinkedList* asLinkedList(ProtoContext* context) const;
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
    static constexpr proto_ulong PROTO_SMALL_INT_TAG_MASK  = PROTO_UL(0x3FF); // pointer_tag(6) + embedded_type(4)
    static constexpr proto_ulong PROTO_SMALL_INT_TAG_VALUE = PROTO_UL(0x001); // POINTER_TAG_EMBEDDED_VALUE | (EMBEDDED_TYPE_SMALLINT << 6)

    /** True iff `obj` is a tagged SmallInteger pointer (no Cell, no allocation). */
    static inline bool isSmallInt(const ProtoObject* obj) {
        return (reinterpret_cast<proto_ulong>(obj) & PROTO_SMALL_INT_TAG_MASK) == PROTO_SMALL_INT_TAG_VALUE;
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
        proto_ulong getSize(ProtoContext* context) const;
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
        proto_ulong getHash(ProtoContext* context) const;
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
        proto_ulong getSize(ProtoContext* context) const;
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
        proto_ulong getHash(ProtoContext* context) const;
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
         * B bytes of text cost `2 * ceil(B / 36) - 1` cells — full 36-byte
         * leaves joined by internal nodes, the handle pointing at the root,
         * about 3.6 bytes of heap per ASCII character — and leave no garbage
         * behind: 7 to 36 bytes are one cell. Up to 6 UTF-8 bytes cost no cell
         * at all: the text lives in the tagged pointer.
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

        /**
         * @brief The canonical, interned form of this string (3.2.0).
         *
         * Returns `this` when the string is already canonical: an inline
         * string (at most INLINE_STRING_MAX_BYTES UTF-8 bytes, canonical by
         * value) or a symbol.  Otherwise returns the symbol with the same
         * content, looked up from this string without building a C string,
         * and interned on first sight.  A lookup of an interned spelling
         * allocates nothing.
         *
         * Attribute names are compared by pointer.  A runtime interns the
         * names it knows at module initialization with createSymbol, and
         * interns a name computed at run time (`obj['a' + '1']`) with this
         * method before the attribute access.
         */
        const ProtoString* intern(ProtoContext* context) const;

        //- Accessors
        const ProtoObject* getAt(ProtoContext* context, int index) const;
        proto_ulong getSize(ProtoContext* context) const;
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
        proto_ulong getHash(ProtoContext* context) const;
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
        proto_ulong getHash(ProtoContext* context) const;
    };

    /** Contiguous buffer (aligned_alloc). Lifecycle tied to descriptor; GC finalize frees segment (Shadow GC). */
    class ProtoExternalBuffer
    {
    public:
        void* getRawPointer(ProtoContext* context) const;
        proto_ulong getSize(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        proto_ulong getHash(ProtoContext* context) const;
    };

    /**
     * @brief A module's identity in the process-global module list:
     *        provider + logical path + version.
     *
     * Ruled by the maintainer on 2026-09-24.  Path alone aliases silently: with
     * the provider prefix stripped, `provider:st/counter_lib` and a local
     * `counter_lib` collapsed into one module with the first load winning for
     * both — the same wrong-answer-no-error class as the 6-versus-7-byte
     * interning bug.
     *
     * Modules are process-level and perennial: they never unload, so two
     * coexisting versions of one module are two modules for the life of the
     * process and identity must tell them apart.  Diamond dependencies will
     * produce exactly that once modules have dependencies.
     *
     * The provider component is the provider's GUID (ModuleProvider::getGUID()),
     * never its alias: an alias is a user-facing nickname that can be re-pointed
     * at a different provider, a GUID is stable.
     *
     * A module that declares NO version has the EMPTY version, and that is a
     * permanent, first-class identity value meaning "declares no version".  It
     * is NOT a wildcard and NOT a synonym for any declared version.  There is no
     * module manifest yet; fixing the key's shape now is what guarantees that
     * introducing one later cannot re-alias any module that exists today, since
     * an unversioned module's key is byte-identical before and after.  A future
     * resolver MUST reject an empty declared version, because "" is reserved.
     */
    class ModuleIdentity
    {
    public:
        ModuleIdentity(std::string providerGUID, std::string logicalPath, std::string version);

        /** provider + path with the empty version: a module that declares none. */
        static ModuleIdentity unversioned(std::string providerGUID, std::string logicalPath);

        const std::string& getProviderGUID() const;
        const std::string& getLogicalPath()  const;
        /** "" means "this module declares no version". */
        const std::string& getVersion()      const;

        /**
         * @brief The canonical single-string key, stable across releases:
         *        providerGUID + '\x1F' + logicalPath + '\x1F' + version.
         *
         * '\x1F' (ASCII unit separator) is the separator because it cannot occur
         * in a GUID, in a POSIX or Windows path, or in any version syntax —
         * unlike '/', which is in every path, and ':', which is in the
         * `provider:` spec and in Windows drive letters.
         */
        const std::string& asKey() const;

        bool operator==(const ModuleIdentity& other) const;

    private:
        std::string providerGUID_;
        std::string logicalPath_;
        std::string version_;
        std::string key_;
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
        proto_ulong nextKey(ProtoContext* context) const;
        const ProtoObject* nextValue(ProtoContext* context) const;
        const ProtoSparseListIterator* advance(ProtoContext* context);
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    class ProtoSparseList
    {
    public:
        bool has(ProtoContext* context, proto_ulong index) const;
        const ProtoObject* getAt(ProtoContext* context, proto_ulong index) const;
        const ProtoSparseList* setAt(ProtoContext* context, proto_ulong index, const ProtoObject* value) const;
        const ProtoSparseList* removeAt(ProtoContext* context, proto_ulong index) const;
        bool isEqual(ProtoContext* context, const ProtoSparseList* otherDict) const;
        proto_ulong getSize(ProtoContext* context) const;

        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoSparseListIterator* getIterator(ProtoContext* context) const;
        proto_ulong getHash(ProtoContext* context) const;

        /**
         * @brief Visit every (key, value) entry in ascending key order.
         *
         * Allocates no Cell and holds NO GC critical section across
         * `method` (up to 3.3.1 the AVL form ran the whole walk, callbacks
         * included, inside one): the callback may allocate, call
         * `safepoint()`, park for a collection, wait on another ProtoThread
         * and run arbitrary embedder code.  For the duration of the walk the
         * receiver is pinned in a root set of the space, so the tree stays
         * reachable even when the caller holds it only in a C++ local -- for
         * instance the result of getOwnAttributes() on a mutable object,
         * whose state a concurrent write can supersede.  An exception thrown
         * by `method` propagates and releases the pin.  The pin lives in the
         * calling thread's own walk-pin root set, so concurrent walks on
         * different threads do not contend (as for
         * ProtoObject::processOwnAttributes: a lock-free set of its own for
         * a thread counted in the space's quorum, otherwise 16 shared sets).
         * An empty list is not pinned.
         */
        void processElements(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, proto_ulong, const ProtoObject*)) const;
        /** @brief processElements without the keys; same guarantees. */
        void processValues(ProtoContext* context, void* self, void (*method)(ProtoContext*, void*, const ProtoObject*)) const;
    };

    /**
     * @class ProtoArguments
     * @brief The positional arguments of one call, as a method reads them.
     *
     * A descriptor over an array of values that the caller builds for the
     * duration of the call and passes by pointer.  Reading never branches on
     * where the values came from: getAt is `values[i]`, inline, and
     * values()/count() give the array itself to a binding loop.  The caller
     * chooses where the array lives and how large it is, and keeps its values
     * reachable for the duration of the call (values held in the caller
     * context's automatic locals are).
     *
     * Reads like a ProtoList: getSize, getAt (PROTO_NONE outside
     * 0..size-1), getFirst, getLast, has.  asList answers the arguments as a
     * ProtoList: the list the call was made with, when it was made with one
     * (ProtoListArguments), or a list built in `context` from the array.  A
     * method that keeps its arguments keeps asList, never the descriptor,
     * which does not outlive the call.
     *
     * See docs/CALLING_CONVENTION.md.
     */
    class ProtoArguments
    {
    public:
        ProtoArguments(const ProtoObject* const* values, unsigned count,
                       const ProtoList* list = nullptr) noexcept
            : values_(values), count_(values ? count : 0), list_(list) {}

        proto_ulong getSize(ProtoContext*) const { return count_; }
        const ProtoObject* getAt(ProtoContext*, int index) const {
            if (static_cast<unsigned>(index) >= count_) return PROTO_NONE;   // also index < 0
            const ProtoObject* v = values_[index];
            return v ? v : PROTO_NONE;
        }
        const ProtoObject* getFirst(ProtoContext* context) const { return getAt(context, 0); }
        const ProtoObject* getLast(ProtoContext* context) const {
            return count_ ? getAt(context, static_cast<int>(count_ - 1)) : PROTO_NONE;
        }
        bool has(ProtoContext* context, const ProtoObject* value) const;
        const ProtoList* asList(ProtoContext* context) const;

        //! The values, for a binding loop: count() of them.
        const ProtoObject* const* values() const { return values_; }
        unsigned count() const { return count_; }

    protected:
        const ProtoObject* const* values_;
        unsigned count_;
        const ProtoList* list_;   // the list the arguments came from, if any
    };

    /**
     * @class ProtoKeywordArguments
     * @brief The keyword arguments of one call, as a method reads them.
     *
     * A descriptor over an array of ProtoKeywordArgument pairs, built by the
     * caller like ProtoArguments.  A key is an interned name's pointer, as
     * ProtoObject::setAttribute keys attributes.  Reads like a
     * ProtoSparseList: getSize, has and getAt by key (PROTO_NONE when
     * absent), processElements in array order.  asSparseList answers the
     * keywords as a ProtoSparseList: the one the call was made with, if any
     * (ProtoSparseListKeywordArguments), or one built in `context`.
     */
    class ProtoKeywordArguments
    {
    public:
        ProtoKeywordArguments(const ProtoKeywordArgument* pairs, unsigned count,
                              const ProtoSparseList* dict = nullptr) noexcept
            : pairs_(pairs), count_(pairs ? count : 0), dict_(dict) {}

        proto_ulong getSize(ProtoContext*) const { return count_; }
        bool has(ProtoContext*, proto_ulong key) const {
            for (unsigned i = 0; i < count_; ++i)
                if (reinterpret_cast<proto_ulong>(pairs_[i].name) == key) return true;
            return false;
        }
        /**
         * @brief The value passed for `key`, or PROTO_NONE when the call
         *        passed no such keyword.
         *
         * PROTO_NONE is also a value a caller can pass (`f(x=None)`), so the
         * result cannot tell a missing keyword from one passed as PROTO_NONE:
         * a callee that must tell them apart (a default applied only when
         * the keyword is absent) asks has() first.
         */
        const ProtoObject* getAt(ProtoContext*, proto_ulong key) const {
            for (unsigned i = 0; i < count_; ++i)
                if (reinterpret_cast<proto_ulong>(pairs_[i].name) == key)
                    return pairs_[i].value ? pairs_[i].value : PROTO_NONE;
            return PROTO_NONE;
        }
        void processElements(ProtoContext* context, void* self,
                             void (*method)(ProtoContext*, void*, proto_ulong, const ProtoObject*)) const {
            for (unsigned i = 0; i < count_; ++i)
                method(context, self, reinterpret_cast<proto_ulong>(pairs_[i].name), pairs_[i].value);
        }
        const ProtoSparseList* asSparseList(ProtoContext* context) const;

        //! The pairs, for a binding loop: count() of them.
        const ProtoKeywordArgument* pairs() const { return pairs_; }
        unsigned count() const { return count_; }

    protected:
        const ProtoKeywordArgument* pairs_;
        unsigned count_;
        const ProtoSparseList* dict_;   // the sparse list the keywords came from, if any
    };

    /**
     * @class ProtoListArguments
     * @brief ProtoArguments over a ProtoList: the list's values copied into
     *        an array for the duration of a call.
     *
     * The bridge for a caller that holds its arguments in a list: the list
     * form of ProtoObject::call uses it, and an embedder that invokes a
     * method pointer with a list does the same.  Up to kInline values stay
     * in the object (on the caller's stack); more go to the heap.  The list
     * keeps the values reachable, so the copy needs no rooting; asList
     * answers the list itself.  `list` may be null (no arguments).
     */
    class ProtoListArguments : public ProtoArguments
    {
    public:
        static constexpr unsigned kInline = 8;
        ProtoListArguments(ProtoContext* context, const ProtoList* list);
        ProtoListArguments(const ProtoListArguments&) = delete;
        ProtoListArguments& operator=(const ProtoListArguments&) = delete;

    private:
        const ProtoObject* inline_[kInline];
        std::vector<const ProtoObject*> heap_;
    };

    /**
     * @class ProtoSparseListKeywordArguments
     * @brief ProtoKeywordArguments over a ProtoSparseList keyed by interned
     *        names' pointers: its entries copied into pairs for a call.
     *
     * The keyword counterpart of ProtoListArguments; up to kInline pairs stay
     * in the object.  asSparseList answers the list itself.  `dict` may be
     * null (no keywords).
     */
    class ProtoSparseListKeywordArguments : public ProtoKeywordArguments
    {
    public:
        static constexpr unsigned kInline = 4;
        ProtoSparseListKeywordArguments(ProtoContext* context, const ProtoSparseList* dict);
        ProtoSparseListKeywordArguments(const ProtoSparseListKeywordArguments&) = delete;
        ProtoSparseListKeywordArguments& operator=(const ProtoSparseListKeywordArguments&) = delete;

    private:
        ProtoKeywordArgument inline_[kInline];
        std::vector<ProtoKeywordArgument> heap_;
    };

    /**
     * @class ProtoLinkedList
     * @brief Immutable, collectable singly linked list, built at the front.
     *
     * Each cell holds up to four elements, the total count and the next
     * cell; every cell is full except the head (the list is aligned at its
     * end), so equal lists have the same cell boundaries and every suffix
     * is itself a list.  Nothing is interned: a list nobody references is
     * garbage.  `asTuple` gives the canonical, interned (perennial) tuple
     * when identity is needed.
     *
     * Costs: `prepend` allocates one cell; `getFirst`, `getSize` and
     * `isEmpty` none; `getRest` none when the head holds one element and one
     * cell otherwise.  `getAt(i)` walks i / 4 cells.  There are no
     * operations at the end and no random access: use ProtoList for those.
     * nullptr is not an element (`prepend(nullptr)` returns the receiver).
     *
     * Since 3.3.0.  See docs/specs/2026-10-25-linked-list-design.md.
     */
    class ProtoLinkedList
    {
    public:
        //- Front access, O(1)
        /** The first element, or nullptr when the list is empty. */
        const ProtoObject* getFirst(ProtoContext* context) const;
        /** Every element but the first; the empty list when size <= 1. */
        const ProtoLinkedList* getRest(ProtoContext* context) const;
        proto_ulong getSize(ProtoContext* context) const;
        bool isEmpty(ProtoContext* context) const;

        //- Construction at the front: one cell
        const ProtoLinkedList* prepend(ProtoContext* context, const ProtoObject* value) const;

        //- Walks of O(k / 4) cells; only the result allocates
        /** The element at `index`, or nullptr when out of range. */
        const ProtoObject* getAt(ProtoContext* context, proto_ulong index) const;
        /** The list without its first `count` elements. */
        const ProtoLinkedList* removeFirst(ProtoContext* context, proto_ulong count) const;
        /** The first `count` elements (ceil(count / 4) cells). */
        const ProtoLinkedList* takeFirst(ProtoContext* context, proto_ulong count) const;

        //- O(n) builders
        /** This list followed by `tail`; `tail` is shared, not copied. */
        const ProtoLinkedList* prependAll(ProtoContext* context, const ProtoLinkedList* tail) const;
        const ProtoLinkedList* reverse(ProtoContext* context) const;

        //- Comparison
        /** Same size and the same element words, in order. */
        bool isEqual(ProtoContext* context, const ProtoLinkedList* other) const;
        /** ProtoTuple's hash of the same elements. */
        proto_ulong getHash(ProtoContext* context) const;

        //- Conversion
        /** A new ProtoList with the same elements. */
        const ProtoList* asList(ProtoContext* context) const;
        /** The interned (perennial) ProtoTuple with the same elements. */
        const ProtoTuple* asTuple(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
    };

    /**
     * @class ProtoLinkedListCursor
     * @brief Walks a ProtoLinkedList without allocating.
     *
     * A value the caller keeps in a C++ local.  It is valid while the list it
     * started from is reachable, as is any `ProtoObject*` in a local.
     * Since 3.3.0.
     */
    class ProtoLinkedListCursor
    {
    public:
        explicit ProtoLinkedListCursor(const ProtoLinkedList* list);
        bool hasNext() const;
        /** The next element, or nullptr when the walk is over. */
        const ProtoObject* next();
    private:
        const void* node;
        proto_ulong index;
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
        proto_ulong getSize(ProtoContext* context) const;

        const ProtoObject* asObject(ProtoContext* context) const;
        const ProtoMapIterator* getIterator(ProtoContext* context) const;
        proto_ulong getHash(ProtoContext* context) const;

        /** Visits (key, value) in ascending key-word order.  Allocates nothing and
         *  holds no GC critical section, so `method` may allocate and run
         *  arbitrary code.  The caller keeps the collection reachable. */
        void processElements(ProtoContext* context, void* self,
            void (*method)(ProtoContext*, void*, const ProtoObject* key, const ProtoObject* value)) const;
        void processValues(ProtoContext* context, void* self,
            void (*method)(ProtoContext*, void*, const ProtoObject* value)) const;
    };

    /**
     * @class ProtoMPSCQueue
     * @brief Mutable, lock-free, multi-producer / single-consumer FIFO of
     *        items the garbage collector traces.
     *
     * Specification: protoScala/docs/platform/PMQ-SPEC.md.  One queue per
     * actor and priority band is the intended use (protoScala DESIGN section 8,
     * protoClojure, protoST).
     *
     * `push` may be called concurrently from any number of threads; it is
     * lock-free, allocates exactly one cell and does no work proportional
     * to the queue's length.  `takeAll` removes every item pushed so far
     * and returns them in push order as an immutable ProtoList; **only one
     * consumer may call it at a time**, which the caller guarantees (an
     * actor runtime already does, through its claimed flag).  Violating
     * that is a caller bug, not undefined memory behaviour: the
     * implementation stays memory-safe and only the partition of items
     * between the racing consumers is unspecified.
     *
     * `takeAll` removes every item pushed so far.  Items pushed *after* the
     * call started may or may not be included: the detach is a single
     * exchange, so a node another producer prepends between the retain
     * publish and the detach is taken by this batch.  An actor runtime must
     * therefore not assume a hard cut-off at the call's entry, only the
     * never-lost / never-duplicated guarantee below.
     *
     * Items are never copied - the queue stores the pointer.  An item
     * pushed before a takeAll begins is returned by that call or a later
     * one: never lost, never duplicated.
     *
     * The queue itself must stay reachable for the duration of a call: the
     * items of a batch being built into a list are reachable through it,
     * and through nothing else.  Hold it in an attribute, a root set or a
     * live context, exactly as for every other protoCore handle.
     *
     * KNOWN LIMITATION (re-measured 2026-09-23 against master a1a8297f, which
     * removed the CriticalSection from ProtoContext::newList).  The large
     * part of this limitation is gone: MPSCQueueGC.LargeDrainDoesNotBlock-
     * StopTheWorld measures the stop-the-world pause (stwFlag up = P1 + P2)
     * while a consumer drains 200,000 items, and its median falls from 88 ms
     * (min 82 ms, max 330 ms over 9 samples, 3 runs) to about 2 ms (min 22
     * us, max 5.3 ms).
     *
     * What remains is smaller and lives HERE, not in the builder: takeAll
     * walks the detached node chain into a std::vector and reverses it, and
     * that loop makes no protoCore call at all, so it never polls the
     * stop-the-world flag.  The pause is therefore still linear in the
     * length of the batch -- median 340 us at 50,000 items, 798 us at
     * 100,000, 2.15 ms at 200,000, 3.67 ms at 400,000, a flat ~0.7% of the
     * drain -- while the plain bulk builder over the same sizes is flat at
     * 27-34 us.  PMQ-SPEC section 3 constraint 1 forbids stop-the-world work
     * proportional to queue length, so the constraint is not yet met; the
     * remaining fix is to poll for a stop-the-world every N nodes of that
     * walk, at criticalSectionDepth 0.
     *
     * Caller contract worth stating explicitly, because a mailbox is the
     * kind of thing a runtime pushes to from a long-running loop: a
     * ProtoContext owns its young generation until it is destroyed, and a
     * cell in a young chain is never a candidate of the running cycle.  A
     * producer that pushes a million messages through ONE context therefore
     * keeps every node out of the collector's reach and will exhaust the
     * heap.  Use one context per turn (the ordinary protoCore model), or
     * call ProtoContext::safepoint() between turns.  This is a property of
     * the context model, not of the queue - but this type makes it easy to
     * trip over, so it is documented here.
     */
    class ProtoMPSCQueue
    {
    public:
        /** Any thread.  Lock-free, O(1), one cell. */
        void push(ProtoContext* context, const ProtoObject* item) const;

        /** Single consumer.  Every queued item in FIFO order; an empty list
         *  when nothing is queued. */
        const ProtoList* takeAll(ProtoContext* context) const;

        /** Any thread; a snapshot that may be stale by the time it is used. */
        bool isEmpty(ProtoContext* context) const;

        const ProtoObject* asObject(ProtoContext* context) const;

        /** Identity hash: the queue is mutable, so its contents cannot
         *  contribute to it. */
        proto_ulong getHash(ProtoContext* context) const;
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
        proto_ulong (*hash)(ProtoContext*, const ProtoObject* key);
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

// ProtoSet keeps every distinct element under a colliding hash, compares by
// pointer, and has hasWithHash / removeWithHash (the release after 3.4.0).
#define PROTOCORE_HAS_SET_BUCKETS 1

    /**
     * @class ProtoSetIterator
     * @brief A persistent cursor over a ProtoSet: visits every element exactly once.
     * @details next() returns the current element and advance() returns a new iterator
     * on the following one; the set is walked bucket by bucket, in no specified order.
     */
    class ProtoSetIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context) const;
        const ProtoSetIterator* advance(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        /**
         * @brief The hash the element next() returns is stored under.
         * @details For an element added with add() it is that element's getHash; for one added
         * with addWithHash() it is the caller's hash.  Elements that share a bucket report the
         * same hash, once per element.
         */
        proto_ulong nextHash(ProtoContext* context) const;
    };

    /**
     * @class ProtoSet
     * @brief An immutable, persistent collection of distinct objects.
     *
     * @details **Equality is pointer identity.**  Two elements are the same element only when
     * they are the same `ProtoObject*`.  For tagged immediates (small integers, booleans,
     * inline strings of up to 6 bytes, Unicode characters) identity is value equality; for
     * heap objects it is not: two distinct heap strings with the same content, two boxed
     * doubles or two large integers of equal value are two elements.  Intern strings
     * (ProtoString::intern) to make equal content one element.
     *
     * Elements are grouped by hash: each hash slot holds a bucket, the list of the distinct
     * elements stored under that hash.  Elements whose hashes collide are therefore all
     * kept; the hash only narrows the search, which then compares pointers inside the
     * bucket.  add(), has() and remove() use ProtoObject::getHash; the `...WithHash`
     * variants take the hash from the caller.  A set must be used with one hash function.
     *
     * Every operation returns a new set that shares structure with the receiver; no
     * operation changes an existing set or bucket.
     *
     * A language whose equality is not identity (content equality for strings, Python's
     * `1 == 1.0 == True`, a user-defined `__eq__`) should not try to express it through
     * ProtoSet: it builds its own structure with the same shape, for example a
     * ProtoSparseList keyed by the language's hash whose values are ProtoLists of the
     * elements, searched with the language's own equality (see also hashedPut and
     * KeySemantics for maps).
     */
    class ProtoSet
    {
    public:
        /**
         * @brief Returns a new set with `value` added under value->getHash().
         * @details Returns the receiver itself when that pointer is already an element.
         */
        const ProtoSet* add(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns PROTO_TRUE if `value` (this pointer) is an element, otherwise PROTO_FALSE.
         */
        const ProtoObject* has(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns a new set without `value` (this pointer); the receiver itself if absent.
         * @details Other elements that share its hash stay.
         */
        const ProtoSet* remove(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns a new set with `value` added to the bucket of `hash`.
         * @details Returns the receiver itself when that pointer is already in that bucket.
         * Elements already stored under `hash` stay: a distinct pointer is appended, never
         * substituted.  A runtime that wants one element per hash tests hasHash() first.
         */
        const ProtoSet* addWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const;

        /**
         * @brief Returns true if `value` (this pointer) is stored in the bucket of `hash`.
         */
        bool hasWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const;

        /**
         * @brief Returns a new set without `value` (this pointer) in the bucket of `hash`.
         * @details The receiver itself if it is not there.  Removing the last element of a
         * bucket removes the bucket.
         */
        const ProtoSet* removeWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const;

        /**
         * @brief Returns true if at least one element is stored under `hash`.
         */
        bool hasHash(ProtoContext* context, proto_ulong hash) const;

        /**
         * @brief Returns a new set without every element stored under `hash` (the whole bucket).
         * @details The receiver itself if no element is stored under `hash`.
         */
        const ProtoSet* removeHash(ProtoContext* context, proto_ulong hash) const;

        /**
         * @brief Returns the number of elements in the set (not the number of hashes).  O(1).
         */
        proto_ulong getSize(ProtoContext* context) const;

        /**
         * @brief Returns the set as a generic ProtoObject.
         */
        const ProtoObject* asObject(ProtoContext* context) const;

        /**
         * @brief Returns an iterator for the set, or nullptr if the set is empty.
         */
        const ProtoSetIterator* getIterator(ProtoContext* context) const;
    };

// ProtoMultiset keeps every distinct element under a colliding hash with its
// own count, compares by pointer, iterates elements (nextCount / nextHash),
// and has has, getDistinctSize and the ...WithHash variants (the release
// after 3.4.0).
#define PROTOCORE_HAS_MULTISET_BUCKETS 1

    /**
     * @class ProtoMultisetIterator
     * @brief A persistent cursor over a ProtoMultiset: visits every distinct element exactly once.
     * @details next() returns the current element (not its count; through 3.4.0 it returned
     * the count) and nextCount() how many times it occurs; advance() returns a new iterator on
     * the following element.  The multiset is walked bucket by bucket, in no specified order.
     * A caller that wants one visit per occurrence repeats each element nextCount() times.
     */
    class ProtoMultisetIterator
    {
    public:
        int hasNext(ProtoContext* context) const;
        const ProtoObject* next(ProtoContext* context) const;
        const ProtoMultisetIterator* advance(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        /**
         * @brief How many times the element next() returns occurs in the multiset (at least 1).
         */
        proto_ulong nextCount(ProtoContext* context) const;
        /**
         * @brief The hash the element next() returns is stored under.
         * @details For an element added with add() it is that element's getHash; for one added
         * with addWithHash() it is the caller's hash.  Elements that share a bucket report the
         * same hash, once per element.
         */
        proto_ulong nextHash(ProtoContext* context) const;
    };

    /**
     * @class ProtoMultiset
     * @brief An immutable, persistent collection that counts occurrences of objects.
     *
     * @details **Equality is pointer identity**, as in ProtoSet.  Two occurrences are of the
     * same element only when they are the same `ProtoObject*`.  For tagged immediates (small
     * integers, booleans, inline strings of up to 6 bytes, Unicode characters) identity is
     * value equality; for heap objects it is not: two distinct heap strings with the same
     * content, two boxed doubles or two large integers of equal value are two elements, each
     * with its own count.  Intern strings (ProtoString::intern) to count equal content as one
     * element.
     *
     * Elements are grouped by hash: each hash slot holds a bucket of the distinct elements
     * stored under that hash, each paired with its count.  Elements whose hashes collide keep
     * separate counts; the hash only narrows the search, which then compares pointers inside
     * the bucket.  add(), has(), count() and remove() use ProtoObject::getHash; the
     * `...WithHash` variants take the hash from the caller.  A multiset must be used with one
     * hash function.
     *
     * getSize() is the number of occurrences (the total of the counts) and getDistinctSize()
     * the number of distinct elements; both are O(1).
     *
     * Every operation returns a new multiset that shares structure with the receiver; no
     * operation changes an existing multiset or bucket.
     *
     * A language whose equality is not identity (content equality for strings, Python's
     * `1 == 1.0 == True`, a user-defined `__eq__`) should not try to express it through
     * ProtoMultiset: it builds its own structure with the same shape, for example a
     * ProtoSparseList keyed by the language's hash whose values are ProtoLists of
     * (element, count) pairs, searched with the language's own equality.
     */
    class ProtoMultiset
    {
    public:
        /**
         * @brief Returns a new multiset with one more occurrence of `value` (this pointer).
         * @details Increments its count, or inserts it with count 1, under value->getHash().
         */
        const ProtoMultiset* add(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns how many times `value` (this pointer) occurs, as a SmallInteger (0 if absent).
         */
        const ProtoObject* count(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns PROTO_TRUE if `value` (this pointer) occurs at least once, otherwise PROTO_FALSE.
         */
        const ProtoObject* has(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns a new multiset with one occurrence of `value` (this pointer) removed.
         * @details Decrements its count; at zero the element is dropped.  Returns the receiver
         * itself if `value` does not occur.  Other elements that share its hash stay.
         */
        const ProtoMultiset* remove(ProtoContext* context, const ProtoObject* value) const;

        /**
         * @brief Returns a new multiset with one more occurrence of `value` in the bucket of `hash`.
         * @details Elements already stored under `hash` keep their counts: a distinct pointer is
         * appended with count 1, never substituted.
         */
        const ProtoMultiset* addWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const;

        /**
         * @brief Returns how many times `value` (this pointer) occurs in the bucket of `hash` (0 if absent).
         */
        proto_ulong countWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const;

        /**
         * @brief Returns true if `value` (this pointer) occurs in the bucket of `hash`.
         */
        bool hasWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const;

        /**
         * @brief Returns a new multiset with one occurrence of `value` removed from the bucket of `hash`.
         * @details The receiver itself if it is not there.  Removing the last occurrence of the
         * last element of a bucket removes the bucket.
         */
        const ProtoMultiset* removeWithHash(ProtoContext* context, proto_ulong hash, const ProtoObject* value) const;

        /**
         * @brief Returns the total number of occurrences (the sum of all counts).  O(1).
         */
        proto_ulong getSize(ProtoContext* context) const;

        /**
         * @brief Returns the number of distinct elements (pointers).  O(1).
         */
        proto_ulong getDistinctSize(ProtoContext* context) const;

        /**
         * @brief Returns the multiset as a generic ProtoObject.
         */
        const ProtoObject* asObject(ProtoContext* context) const;

        /**
         * @brief Returns an iterator over the distinct elements, or nullptr if the multiset is empty.
         */
        const ProtoMultisetIterator* getIterator(ProtoContext* context) const;
    };

    class ProtoByteBuffer
    {
    public:
        proto_ulong getSize(ProtoContext* context) const;
        char* getBuffer(ProtoContext* context) const;
        char getAt(ProtoContext* context, int index) const;
        void setAt(ProtoContext* context, int index, char value);
        const ProtoObject* asObject(ProtoContext* context) const;
        proto_ulong getHash(ProtoContext* context) const;
    };

    class ProtoThread
    {
    public:
        /**
         * @brief The thread `context` runs on (`context->thread`): the
         *        ProtoThread whose body is executing, or the space's main
         *        thread.  Null when `context` is null or has no thread.
         *        Defined since 3.4.0 (declared, but failed to link, before).
         */
        static const ProtoThread* getCurrentThread(ProtoContext* context);

        void detach(ProtoContext* context);

        /**
         * @brief Wait for this thread to finish.
         *
         * **The caller does NOT need to wrap this in an `UnmanagedScope`:
         * `join` leaves the running set itself for the duration of the
         * block.**  That is load-bearing rather than a convenience.  The main
         * thread is counted in `runningThreads` from `ProtoSpace`
         * construction and every managed thread adds one, while a
         * stop-the-world phase cannot begin until
         * `parkedThreads >= runningThreads`.  A registered thread blocked in a
         * bare `std::thread::join` reaches no safepoint, so it still counts as
         * running: the quorum can never be met, no cycle can start, and every
         * thread that then needs memory waits for a cycle that cannot begin —
         * usually including the thread being joined.  That is a deadlock, not
         * slow shutdown.
         *
         * Wrapping the call in an `UnmanagedScope` as well is harmless and
         * idempotent (`unmanagedDepth` is a counter; only the outermost pair
         * moves `parkedThreads`), so existing embedder guards need not be
         * removed.
         *
         * **The one exception, and it is the caller's bug:** when the calling
         * context is inside a `ProtoContext::CriticalSection`
         * (`criticalSectionDepth > 0`), `join` does NOT leave the running set,
         * because a thread in a critical section holds cells reachable only
         * from C++ locals and a stop-the-world root scan would miss them.
         * Leaving the running set there would trade a deadlock for memory
         * corruption.  The join still happens, the quorum is still held for
         * its duration, and a one-time diagnostic is written to `stderr`.
         * Do not block inside a critical section: see
         * `docs/EMBEDDER-CONFORMANCE.md` rule 12.
         *
         * Note that this covers `ProtoThread::join` only.  A runtime that
         * calls `std::thread::join` directly on a thread it registered with
         * protoCore still has to bracket that call itself — the kernel cannot
         * see it (rule 2, case `stw.quorum_completes`).
         */
        void join(ProtoContext* context);
        void exit(ProtoContext* context);

        const ProtoObject* getName(ProtoContext* context) const;
        const ProtoObject* asObject(ProtoContext* context) const;
        proto_ulong getHash(ProtoContext* context) const;

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

        //- Waiting (3.1.0)
        /**
         * @brief Gives up the processor once, as `std::this_thread::yield`,
         *        without holding up a collection.
         *
         * The calling thread leaves the stop-the-world quorum of every space
         * it belongs to for the duration of the yield (as `join` does) and,
         * on the way back, parks if a stop-the-world is in progress and
         * clears its caches, exactly as at a safepoint.  A thread that polls
         * for a condition another thread will establish -- a lazy object
         * claimed by another thread (`ProtoObject::claimLazy`), a promise
         * not yet delivered -- calls this or `sleepFor` between polls, so
         * that a collection the other thread needs can run meanwhile.  A
         * runtime never calls `std::this_thread::yield` or `sleep_for`
         * itself for that: a registered thread in a bare OS wait reaches no
         * safepoint and stalls every stop-the-world of its spaces.
         *
         * `context` is a context of the calling thread.  The caller holds no
         * `ProtoObject*` only in C++ locals across the call (a collection may
         * run during it): what it still needs is in a slot of a context, a
         * pin or another root.  Inside a `ProtoContext::CriticalSection` the
         * thread cannot leave the quorum; the call then yields while holding
         * it, so no collection can start, and says so once on `stderr`
         * (docs/EMBEDDER-CONFORMANCE.md rule 12).  A `nullptr` context, or a
         * context with no thread, yields with no bracket: such a thread is in
         * no quorum.
         */
        static void yield(ProtoContext* context);

        /**
         * @brief Blocks the calling thread for at least `microseconds`, off
         *        the stop-the-world quorum, as `yield` does.  `0` is `yield`.
         */
        static void sleepFor(ProtoContext* context, proto_ulong microseconds);
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
        // Where automaticLocals lives (a SlotsSource): an embedder's buffer
        // (e.g. a frame's stack SBO), a new[] array the destructor frees, or
        // the thread's context slot stack, to which the destructor returns
        // it.  One byte, as the bool it replaced.
        std::uint8_t slotsSource_;
        enum SlotsSource : std::uint8_t { kSlotsExternal = 0, kSlotsHeap = 1, kSlotsStack = 2 };
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
         * @param args The positional arguments passed to the function: the
         *        descriptor the method received, or one built over a list or
         *        an array.  Binding reads it; nothing is copied.
         * @param kwargs The keyword arguments passed to the function, likewise.
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
            const ProtoArguments* args = nullptr,
            const ProtoKeywordArguments* kwargs = nullptr,
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
         * If `newCount > automaticLocalsCount`, the array grows to
         * `newCount` slots, the new ones PROTO_NONE: in place when it is
         * the top of the thread's context slot stack, otherwise into a
         * fresh array from that stack (or new[] when the stack has no
         * room), with the existing entries copied and the previous array
         * released (an embedder's buffer is left alone).  Either way the
         * context owns the result and its destructor releases it.
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
        /**
         * @brief Wraps a C function pointer, optionally bound to a receiver.
         *
         * `self` is a **strong, traced** reference: `ProtoMethodCell` reports
         * it unconditionally from `processReferences`, so the cell keeps
         * `self` alive for as long as the cell itself is reachable.
         *
         * @warning Installing the returned cell as an attribute OF `self`
         * creates a reference cycle that protoCore never collects, because a
         * cycle through a mutable object is permanent: the mutables table
         * originates marking and an entry is released only once its handle
         * has been finalized.  See docs/MemoryModel.md section 7.  Three
         * runtimes in this family have done it by accident, and in two of
         * them the binding was never read.
         *
         * Pass `nullptr` for `self` unless the callee actually needs the
         * receiver — protoCore itself never invokes a method cell, so
         * whether `self` is ever read is entirely the embedder's choice, and
         * an unread binding is pure retention.  Where the receiver IS needed,
         * declare the resulting cycle through the conformance rule rather
         * than leaving it undeclared (docs/EMBEDDER-CONFORMANCE.md, rule 13).
         */
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
        const ProtoObject* fromBuffer(proto_ulong length, char* buffer, bool freeOnExit = false);
        const ProtoObject* newBuffer(proto_ulong length);
        const ProtoObject* fromBoolean(bool value);
        const ProtoObject* fromByte(char c);
        const ProtoObject* fromDate(unsigned year, unsigned month, unsigned day);
        const ProtoObject* fromTimestamp(proto_ulong timestamp);
        const ProtoObject* fromTimeDelta(proto_long timedelta);

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
        /** Empty lock-free MPSC queue (PMQ-SPEC section 2.1). */
        const ProtoMPSCQueue* newMPSCQueue();
        /** The empty ProtoLinkedList (one per process). Since 3.3.0. */
        const ProtoLinkedList* newLinkedList();
        /**
         * A ProtoLinkedList holding `values[0..count)` in order: ceil(n / 4)
         * cells, the cheapest way to return several values.  nullptr entries
         * are skipped.  Since 3.3.0.
         */
        const ProtoLinkedList* newLinkedListFromArray(proto_ulong count, const ProtoObject* const* values);
        /** A ProtoLinkedList with the elements of `list`, in order. Since 3.3.0. */
        const ProtoLinkedList* newLinkedListFromList(const ProtoList* list);
        /** A ProtoLinkedList with the elements of `tuple`, in order. Since 3.3.0. */
        const ProtoLinkedList* newLinkedListFromTuple(const ProtoTuple* tuple);
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

        /**
         * @brief Throws a `ProtoException` carrying `value`.  The language
         *        equivalent of `throw value`; see docs/EXCEPTIONS.md.
         */
        [[noreturn]] void raise(const ProtoObject* value);

        /**
         * @brief Raises a protoCore error: builds its value with the space's
         *        error factory (ProtoSpace::setErrorFactory) and throws it as
         *        a `ProtoException`.
         */
        [[noreturn]] void raiseError(ProtoErrorKind kind, const char* message);

        /** Allocates a contiguous buffer (aligned_alloc). GC finalize frees it when descriptor is collected (Shadow GC). */
        const ProtoObject* newExternalBuffer(proto_ulong size);
        /**
         * Create a fresh, GC-owned ProtoByteBuffer holding `len` raw octets.
         * The bytes are copied from `data` (data may be null only if len == 0).
         * Unlike ProtoString, ProtoByteBuffer is opaque-binary: every byte
         * 0..255 round-trips, and embedded nulls do not truncate.
         */
        const ProtoByteBuffer* newByteBuffer(const char* data, proto_ulong len);

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
         *
         * **IT IS ALSO THE ONLY PLACE A CONTEXT'S YOUNG GENERATION IS
         * SUBMITTED** to `dirtySegments` (under
         * `PROTOCORE_GC_REINCLUDE_SURVIVORS`, once the per-context allocation
         * threshold `ProtoSpace::maxAllocatedCellsPerContext` has been
         * crossed, and only at `criticalSectionDepth == 0`).  A context's young
         * chain is recorded by GC Phase 2 as a ROOT, not as a candidate, so a
         * chain that is never submitted is live by construction and no cycle
         * can ever reclaim it — however many cycles run.  An interpreter that
         * never calls `safepoint()` therefore reclaims NOTHING, silently,
         * while `gcCycleCount` keeps advancing and the heap keeps growing.
         *
         * This half of the contract was undocumented until 2026-09-25, and its
         * absence is a plausible contributing cause of two measured bugs:
         * protoST reclaimed 0 cells of 2,748,398 across its entire history
         * with its whole suite green (848/848 when S15 was measured; the 833
         * once quoted here was the earlier S13 state), and protoClojure's
         * apparent live set was 90.8× its real one — 196,519 cells against
         * 2,164 at a 400,000-cell ceiling, the pair recorded in
         * `protoClojure/tests/cli/loop-garbage-is-reclaimed.sh`.  (An earlier
         * "110×" had no operand pair; see `docs/FIELD-NOTES.md`, case 2.)
         * An embedder reading only the paragraph above would
         * conclude that a CPU-bound loop needs a safepoint and an allocating
         * loop does not, which is the opposite of the truth for reclamation.
         *
         * See `docs/EMBEDDER-CONFORMANCE.md` rule 1 and conformance case
         * `gc.young_submitted`.
         *
         * **The obligation that comes with it: call it only where every cell
         * this context allocated and still uses is reachable from a root**
         * (automatic locals, a `ProtoRootSet`, the mutables table, a
         * prototype).  Until the submission, the young chain protects a cell
         * held only in a C++ local across any number of allocations; from the
         * submission on, nothing does, and the next cycle frees it.  So the
         * window opens at the embedder's own safepoint, not at an allocation,
         * and code that is correct by rule 3's usual reading still breaks.
         * protoPython's call frame was held this way for its whole history,
         * and a nested function lost its enclosing variables once cycles ran
         * (rule 3, `docs/EMBEDDER-CONFORMANCE.md`).  Never call it on a
         * context another activation is using, such as a caller's context
         * passed down to generated code: that activation's C++ locals are not
         * anchored at your call.
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
        proto_ulong allocatedCellsCount;
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
        std::atomic_flag lock = ATOMIC_FLAG_INIT;

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
        proto_ulong size() const;

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
        // protoCore's own walk pins: a root set only one thread ever writes
        // (headers/proto_internal.h, ProtoSpaceImplementation).
        friend ProtoRootSet* createSingleWriterRootSet(ProtoSpace* space, const char* name);
        ProtoRootSet(ProtoSpace* owner, const char* name);
        ~ProtoRootSet();

        struct Impl;
        Impl* impl_;
    };

    /**
     * @brief One cycle of the mutable-reference graph: a set of mutable
     *        objects whose mutables-table entries keep each other alive for
     *        the lifetime of the space.
     *
     * Until 3.1.0 a cycle among mutables was never collected: the whole
     * mutables table was a root, so a value that reached a handle that reached
     * it back kept both alive ([docs/MemoryModel.md](../docs/MemoryModel.md)
     * § 7).  Since 3.1.0 the table's states are not roots
     * ([docs/GLOBAL_MUTABLE_TABLE.md](../docs/GLOBAL_MUTABLE_TABLE.md)), so an
     * unreachable cycle is collected and a cycle found in the table is
     * reachable, or awaits the next cycle.
     *
     * `refs` is the complete set of `mutable_ref`s in the cycle.  `path` is a
     * SHORTEST closed walk through the lowest-numbered one, because a cycle with
     * no path is not actionable: it names the attribute at every hop it can
     * name.  For a large component the walk is therefore the tightest loop
     * inside it and not a tour of every member -- `refs` is what says how big it
     * is.  (A type system whose every class holds its own `__mro__` and is held
     * by its base's subclass list is one component of hundreds of handles, and a
     * path through all of them would be unreadable.)
     */
    struct MutableCycle
    {
        std::vector<proto_ulong> refs;   ///< the mutable_refs, ascending
        std::string                path;   ///< a closed walk, hop by hop
    };

    /**
     * @brief The result of `ProtoSpace::findMutableCycles`.
     *
     * Data, not a printout, and not a boolean: the diagnostic is worth having
     * only if it says *which* handles and *by which path*, and the caller's own
     * test framework does the asserting.  The counters are carried on a clean
     * result too — a report whose numbers are all zero is a broken scan that
     * looks like a clean graph, and that confusion is the reason this whole
     * family of checks exists.
     */
    struct MutableGraphReport
    {
        proto_ulong handles      = 0;  ///< entries in the mutables table
        /**
         * References TO a mutable handle found anywhere in the walked graph.
         * Not the number of cycles' worth of anything: it is the measure that
         * says the scan was looking at a real object graph, so that an empty
         * `cycles` with a zero here is recognisable as a scan that saw nothing
         * rather than as a graph that is clean.
         */
        proto_ulong handleReferences = 0;
        proto_ulong cellsVisited = 0;  ///< cells the scan walked
        /**
         * True when the cell budget ran out.  A cycle reported by a truncated
         * scan is still real — an edge the scan found is an edge that exists —
         * but the ABSENCE of cycles is not established.  Never read an empty
         * `cycles` as a clean bill of health while this is set.
         */
        bool truncated = false;
        std::vector<MutableCycle> cycles;

        /** True when the graph is acyclic AND the scan was complete. */
        bool acyclicAndComplete() const { return cycles.empty() && !truncated; }

        /** One line of counters, then one line per cycle.  Never empty. */
        std::string summary() const;
    };

    /**
     * @brief Configuration of the adaptive heap controller
     *        (ProtoSpace::enableAdaptiveHeap).  Since 2.10.0; test for it with
     *        PROTOCORE_HAS_ADAPTIVE_HEAP.
     *
     * Every field has a working default, so `space.enableAdaptiveHeap()` with
     * no argument is the intended call.  See docs/GarbageCollector.md
     * § "Adaptive heap controller".
     */
    struct AdaptiveHeapConfig
    {
        /**
         * Hard limit H, in cells: a process-wide safety cap on the sum of all
         * spaces' heaps.  0 selects the automatic value: 75 % of the smaller
         * of physical memory and the process memory limit (cgroup v1/v2 on
         * Linux, job object on Windows), divided by the 64-byte cell.
         * PROTOCORE_HEAP_LIMIT_CELLS, when set, takes precedence.  Clamped to
         * INT_MAX cells (128 GiB) because cell counts are `int` in ABI 3.
         */
        proto_ulong hardCells = 0;
        /** Initial soft limit S0, in cells.  0 selects 2,097,152 (128 MiB;
         *  32 MiB in 2.10.0).  A value at or above the hard limit starts at
         *  the budget (clamped to H): for an embedder that knows its run fits
         *  (since 2.13.0 the controller never grows to the budget on
         *  speculation). */
        proto_ulong initialSoftCells = 0;
        /** Ignored since 2.13.0 (the 2.10.1 law's p_high; the control law has
         *  no fitted constant now).  Kept: the layout of this struct is ABI. */
        double highPressure = 0.05;
        /** Ignored since 2.13.0 (the 2.10.1 law's growth factor g; the law
         *  doubles S while doubling reduces the mutators' waits). */
        double growthFactor = 1.5;
        /** Ignored since 2.13.0 (the 2.10.1 law's k_live). */
        double liveHeadroom = 3.0;
    };

    /**
     * @brief Attribute operations that received a non-interned name
     *        (ProtoSpace::nonInternedNameCounts).
     *
     * An attribute key is the address of an interned symbol.  A name passed
     * as a plain heap string (POINTER_TAG_STRING) is canonicalised first --
     * looked up (get, has, hasOwn) or interned (the writes) in the symbol
     * table -- which costs a content hash and a trie walk on every call.  The
     * counts show how often a runtime does that, so it can intern its names
     * earlier.  Process-wide, since start; inline (<= 6 byte) names and
     * symbols are never counted.
     */
    struct NonInternedNameCounts {
        proto_ulong get = 0;          // getAttribute
        proto_ulong has = 0;          // hasAttribute
        proto_ulong hasOwn = 0;       // hasOwnAttribute
        proto_ulong set = 0;          // setAttribute
        proto_ulong setBulk = 0;      // setAttributes
        proto_ulong setIfEqual = 0;   // setAttributeIfEqual
        proto_ulong remove = 0;       // removeAttribute
        proto_ulong total() const {
            return get + has + hasOwn + set + setBulk + setIfEqual + remove;
        }
    };

    /** @brief A snapshot of a space's heap controller (adaptiveHeapStats). */
    struct AdaptiveHeapStats
    {
        bool        enabled = false;          ///< the controller drives the limits
        proto_ulong softCells = 0;            ///< current soft limit S
        proto_ulong hardCells = 0;            ///< hard limit H (process budget)
        proto_ulong liveCellsLastCycle = 0;   ///< L of the last completed cycle
        proto_ulong heapCells = 0;            ///< this space's heap (heapSize)
        uint64_t    cycles = 0;               ///< cycles completed while enabled
        /** The mutators' wait share w of the last interval (time waiting
         *  for headroom per thread over the interval; since 2.13.0.  Before,
         *  p = (stop-the-world + waits) / interval, summed over threads). */
        double      lastPressure = 0.0;
    };

// ProtoSpace::enableAdaptiveHeap and adaptiveHeapStats exist (2.10.0+).
#define PROTOCORE_HAS_ADAPTIVE_HEAP 1
// ProtoSpace::setCollectorHelperThreads / collectorHelperThreads exist (2.13.0+).
#define PROTOCORE_HAS_COLLECTOR_HELPERS 1

    class ProtoSpace
    {
    public:
        explicit ProtoSpace();
        /**
         * Destroys the space.  Join its ProtoThreads first.  A thread still
         * running at destruction (a daemon thread at process exit) is
         * stopped for good instead: the destructor stops the world one last
         * time and never resumes it.  Such a thread blocks at its next
         * safepoint, allocation or heap wait -- or, if it is out in an
         * unmanaged region, when it returns -- and never runs code of the
         * space again; joining it never returns.  The destructor waits for
         * each such thread to reach one of those points, as a collection
         * does.  A context without a thread used after its space is
         * destroyed is still undefined behaviour.
         */
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
        ProtoObject* mpscQueuePrototype{};

        // --- Cached Literals ---
        ProtoString* literalData;
        ProtoString* literalSetAttribute;
        ProtoString* literalCallMethod;

        //- Callbacks
        //! A call whose name resolves to a value that is not a method.  It
        //! receives the call's arguments as descriptors, as a method would.
        ProtoObject* (*nonMethodCallback)(
            ProtoContext* context,
            const ParentLink* nextParent,
            const ProtoString* method,
            const ProtoObject* self,
            const ProtoArguments* positionalParameters,
            const ProtoKeywordArguments* keywordParameters){};

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
        
        void deallocMutable(proto_ulong mutable_ref);

        /**
         * @brief Find every cycle in this space's mutable-reference graph.
         *
         * Until 3.1.0 **a cycle among mutable objects was never collected**,
         * and this detector was written for that condition; since 3.1.0 an
         * unreachable cycle is collected (docs/GLOBAL_MUTABLE_TABLE.md,
         * "States are not roots") and a cycle it reports is reachable, or
         * awaits the next cycle.  It waits until no cycle is between its
         * stop-the-world and its release of dead entries.  It is an
         * exact, not heuristic, detector, and it is exact
         * because the mutables table enumerates every handle in the space:
         * there is nowhere for a mutable to hide.  See
         * [docs/MemoryModel.md](../docs/MemoryModel.md) § 7 for the property,
         * the reasoning and the rule; conformance rule 13 in
         * [docs/EMBEDDER-CONFORMANCE.md](../docs/EMBEDDER-CONFORMANCE.md) makes
         * it normative.
         *
         * Three lines are enough for any embedder:
         * @code
         *   const proto::MutableGraphReport r = space.findMutableCycles(ctx);
         *   if (!r.cycles.empty()) std::cerr << r.summary();
         * @endcode
         *
         * **Only cycles are reported.**  An acyclic handle-to-handle edge is
         * NOT a violation and is deliberately not reported: if `H1 -> H2` with
         * no cycle, `H2`'s liveness merely follows `H1`'s, and once `H1` dies
         * `H2`'s entry is released with it.  Reporting those would bury
         * the real finding.
         *
         * **Cost and safety.**  It walks every cell reachable from the table, so
         * it is O(live mutable graph) and belongs in a test or a diagnostic
         * build, not in a hot path.  It allocates no `Cell`, so it cannot
         * itself add a handle or trigger a collection.  It holds a
         * `ProtoContext::CriticalSection` on `context` for the whole walk, which
         * is what makes the walk safe: a thread inside a critical section does
         * not park, so no new stop-the-world can begin, so no new sweep can free
         * a cell under the scan.  A cycle already in flight cannot free anything
         * the scan can see either — everything reachable from the live table was
         * either marked at that cycle's stop-the-world or allocated after it,
         * and neither is a candidate of that cycle.
         *
         * The corollary is that this call **stalls the collector** for its
         * duration, and on a space at its heap ceiling that stall is the
         * difference between waiting and the out-of-memory path.  Run it at a
         * quiescent point.
         *
         * @param context a context of the CALLING thread.  Passing `nullptr` or
         *        a context of another thread performs the same walk without the
         *        critical section, which is correct only when the caller can
         *        guarantee by other means that no collection will start — as the
         *        collector's own end-of-cycle hook can, being the only thread
         *        that starts one.
         * @param cellBudget the maximum number of cells to walk before giving
         *        up and setting `MutableGraphReport::truncated`.  0 selects the
         *        default (8,000,000 cells, ~1 s on the reference machine).
         */
        MutableGraphReport findMutableCycles(ProtoContext* context,
                                            proto_ulong cellBudget = 0) const;

        /**
         * @brief The number of `ProtoSpace` instances live in this process
         *        (constructed and not yet destroyed), counting this one.
         *
         * One space per process is the recommended model
         * (docs/GLOBAL_MUTABLE_TABLE.md, "Recommended model"): every runtime,
         * thread, worker and cross-runtime module of the process shares it.
         * Several spaces are supported but not recommended; while more than
         * one is live, the mutable table's states are roots of every cycle.
         * A runtime or a test can use this to check that it created no space
         * of its own.  Takes the process-wide lock briefly; not for hot paths.
         */
        static proto_ulong liveSpaceCount();

        //! Declared, not implemented: no definition exists in the library
        //! (3.4.0), so a call fails to link.  Kept for ABI stability.
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
         *
         * Fixed limits: a call disables the adaptive heap controller
         * (enableAdaptiveHeap) of this space.
         */
        void setHeapLimits(int softCells, int hardCells);

        /**
         * @brief Let protoCore size this space's heap (since 2.10.0).
         *
         * Objective (since 2.13.0): minimise the time the mutators lose
         * waiting for the collector, within the budget H.  The soft limit S,
         * which triggers collections, starts at `initialSoftCells` (128 MiB
         * by default) and is set by the collector at the end of every cycle,
         * from measurements only: while the mutators wait and the collector
         * keeps up (allocation rate r below its throughput T), S becomes the
         * headroom a cycle needs to run behind them, L + G* with
         * G* = rho L / (1 - rho), rho = r / T; while they wait and it does
         * not keep up, S doubles as long as each doubling reduces the waits,
         * and stops after two that do not.  S never decreases and never
         * exceeds H.  The hard limit H is a process-wide budget on the sum of
         * all spaces' heaps; out of memory is declared only when the live
         * set a full cycle marked (plus the other spaces' heaps) does not fit
         * under it, or when cycles at the ceiling reclaim nothing (since
         * 2.14.4; before, the cells a cycle left unreclaimed, which include
         * threads' batches and calls in progress, were the test).
         *
         * One call at start-up replaces a runtime's fixed limit:
         * @code
         *   space.enableAdaptiveHeap();
         * @endcode
         *
         * Environment: PROTOCORE_HEAP_LIMIT_CELLS=<hard> or <soft>,<hard>
         * overrides H (and S0); PROTOCORE_ADAPTIVE_HEAP_START=budget starts
         * S at H (since 2.13.0); PROTOCORE_HEAP_TRACE=1 prints one line per
         * cycle to stderr; PROTOCORE_ADAPTIVE_HEAP=0 applies the hard limit as
         * a fixed one instead (setHeapLimits(0, H)), for diagnosis;
         * PROTOCORE_ADAPTIVE_HEAP=1 enables the controller in every space at
         * its creation and makes setHeapLimits leave it on, to measure an
         * existing embedder under it (diagnosis, since 2.10.1).
         *
         * Thread-safe.  Memory is never returned to the operating system.
         * See docs/GarbageCollector.md § "Adaptive heap controller".
         */
        void enableAdaptiveHeap(const AdaptiveHeapConfig& config = AdaptiveHeapConfig());

        /** @brief The controller's current state (thread-safe snapshot). */
        AdaptiveHeapStats adaptiveHeapStats() const;

        /**
         * @brief Set the number of collector helper threads (process-wide).
         *
         * While mutators wait for heap headroom, up to this many helper
         * threads sweep alongside the collector thread
         * (docs/GarbageCollector.md § "Parallel sweep").  0 sweeps on the
         * collector thread alone, as before 2.13.0.  The default is half the
         * machine's physical cores; PROTOCORE_GC_SWEEP_THREADS sets it
         * without code (0..64), and this call overrides both.  At most 64.
         * Helpers are created when a sweep first wants them and stopped when
         * the last ProtoSpace is destroyed.  Since 2.13.0; test for it with
         * PROTOCORE_HAS_COLLECTOR_HELPERS.
         */
        static void setCollectorHelperThreads(unsigned count);
        /** @brief The collector helper thread count in force. */
        static unsigned collectorHelperThreads();

        /**
         * @brief When the helpers are offered a sweep (process-wide).
         *
         * 0 (default, "measured"): while a mutator waits for heap headroom,
         * and only while that shortens the sweep -- a sweep with helpers that
         * is not faster per cell than the last one without them holds them
         * back for the next 1, 2, 4 ... 64 sweeps.  1 ("waiting"): while a
         * mutator waits.  2 ("always"): every sweep (diagnosis).  -1 restores
         * the default.  PROTOCORE_GC_SWEEP_ENGAGE=measured|waiting|always sets
         * it without code.  Since 2.14.0.
         */
        static void setCollectorHelperEngagement(int mode);
        static int collectorHelperEngagement();

        /**
         * @brief How many segment chains the sweep walks in lockstep, and
         *        whether it prefetches their next cells (process-wide).
         *
         * Hardware-sensitive: several chains keep several cache misses in
         * flight on one thread; the default (8 chains, prefetch on) was
         * measured on one notebook-class CPU only.  1..32 chains; 0 restores
         * the default.  Prefetch: 1 on, 0 off, -1 the default.
         * PROTOCORE_GC_SWEEP_CURSORS and PROTOCORE_GC_SWEEP_PREFETCH set them
         * without code.  Since 2.14.0.
         */
        static void setSweepCursors(unsigned count);
        static unsigned sweepCursors();
        static void setSweepPrefetch(int on);
        static bool sweepPrefetch();

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

        //- Errors (see docs/EXCEPTIONS.md)
        /**
         * @brief Installs the factory that builds the value of every error
         *        protoCore raises in this space; nullptr restores the default.
         *
         * Install it once, while the runtime starts and before any other
         * thread runs code in the space.  The default factory answers a child
         * of `getErrorPrototype(context, kind)` with a `message` attribute.
         */
        void setErrorFactory(ProtoErrorFactory factory);
        /** @brief The installed factory, or nullptr for the default one. */
        ProtoErrorFactory getErrorFactory() const;

        /**
         * @brief protoCore's own prototype for errors of `kind`, created on
         *        first use and alive for the space's lifetime.
         *
         * Every kind's prototype is a child of `getErrorRootPrototype`.  Each
         * carries a `name` attribute: the kind's name (`errorKindName`).
         */
        const ProtoObject* getErrorPrototype(ProtoContext* context, ProtoErrorKind kind);
        /** @brief The parent of every protoCore error prototype ("ProtoCoreError"). */
        const ProtoObject* getErrorRootPrototype(ProtoContext* context);
        /** @brief "InvalidArgument", "TypeMismatch", "Overflow", "DivisionByZero", "ArityMismatch". */
        static const char* errorKindName(ProtoErrorKind kind);

        /**
         * @brief The prototype of every ProtoLinkedList of this space.
         *
         * An accessor rather than a public field, so that the layout of
         * ProtoSpace stays that of 3.2.0.  Since 3.3.0.
         */
        ProtoObject* getLinkedListPrototype() const;
        void setLinkedListPrototype(ProtoObject* prototype);

        //- Thread Management
        //
        // Declared, not implemented: allocThread, deallocThread, getThreads
        // and getThreadByName have no definition in the library (3.4.0), so a
        // call fails to link.  Threads are created with newThread and listed
        // in `threads`.  getCurrentThread is defined (since 3.4.0).
        void allocThread(ProtoContext* context, const ProtoThread* thread);     //!< Declared, not implemented.
        void deallocThread(ProtoContext* context, const ProtoThread* thread);   //!< Declared, not implemented.
        const ProtoList* getThreads(ProtoContext* context);                     //!< Declared, not implemented.
        //! Same as ProtoThread::getCurrentThread(context).  Defined since 3.4.0.
        const ProtoThread* getCurrentThread(ProtoContext* context);
        const ProtoThread* getThreadByName(ProtoContext* context, const ProtoString* threadName);   //!< Declared, not implemented.

        /** Returns the current resolution chain (ProtoList of ProtoString). Default is platform-dependent if not set. */
        const ProtoObject* getResolutionChain() const;
        /** Sets the resolution chain. \a newChain must be a ProtoList of ProtoString; if null or invalid, restores default chain. */
        void setResolutionChain(const ProtoObject* newChain);
        /** Resolve and load a module by \a logicalPath using this space's resolution chain. Returns a wrapper object with attribute \a attrName2create pointing to the module, or PROTO_NONE. Thread-safe; uses SharedModuleCache. */
        const ProtoObject* getImportModule(ProtoContext* context, const char* logicalPath, const char* attrName2create);

        /**
         * @brief Register `module` as a process-global module root owned by this
         *        space.
         *
         * P3: the module list is global and is a GC root.  The entry records this
         * space as the owner of the module's cells, so this space's collector —
         * and only this space's — traces it.  Entries are never removed: a loaded
         * module is perennial, for the process.  Anything that must be UNPINNED
         * belongs in a ProtoRootSet (see `createRootSet`), not here.
         *
         * Thread-safe.  Allocates no Cell.
         */
        void addModuleRoot(const ProtoObject* module);

        /** @brief Module roots this process holds.  Diagnostics and tests. */
        static proto_ulong moduleRootCount();

        /**
         * @brief How many attribute operations received a non-interned name
         *        (NonInternedNameCounts).  Relaxed reads of process-wide
         *        counters; the counting costs one relaxed increment on the
         *        already slow path of such a call.
         *
         * PROTOCORE_NONINTERNED_NAMES adds the call sites: set to "1" (or
         * "stderr") it writes, at process exit, the counts and the 20 most
         * frequent callers (return addresses, symbolised where the platform
         * can) to stderr; any other value is a file path the report is
         * appended to.  Unset, it costs nothing beyond the counters.
         */
        static NonInternedNameCounts nonInternedNameCounts();

        /**
         * @brief The stack size of the threads newThread creates, from now on.
         *
         * 0 (the default) means the platform's size: on Linux the glibc
         * default (RLIMIT_STACK, usually 8 MiB, or what
         * pthread_setattr_default_np set), on Windows the executable's /STACK
         * reserve (1 MiB unless the linker was told otherwise), on macOS
         * 512 KiB whatever the main thread has.  A non-zero value is honoured
         * on every platform since 2.9.0 (on macOS only, in 2.8.0): newThread
         * runs the thread on a native thread created with at least this many
         * bytes of stack (rounded up to whole pages, and to the platform's
         * minimum; on Windows it is the reservation, as /STACK is).  If the
         * platform refuses a thread of that size, newThread says so once on
         * stderr and runs the thread with the platform's size instead; an
         * embedder that must know can ask, from inside the thread,
         * currentThreadStackBytes().
         *
         * Process-wide; call it once at start-up, before creating threads.
         * Since 2.8.0; test for it with PROTOCORE_HAS_THREAD_STACK_BYTES.
         */
        static void setThreadStackBytes(size_t bytes);
        static size_t threadStackBytes();

        /**
         * @brief The stack size of the calling thread, as the platform reports
         *        it, in bytes.
         *
         * The reservation on Windows (GetCurrentThreadStackLimits), the pthread
         * stack size on Linux (pthread_getattr_np) and macOS
         * (pthread_get_stacksize_np); 0 on a platform that offers no way to
         * ask.  Called from a thread newThread created after
         * setThreadStackBytes(n), it is at least n unless the platform refused
         * that size.  Since 2.9.0; test for it with
         * PROTOCORE_HAS_CURRENT_THREAD_STACK_BYTES.
         */
        static size_t currentThreadStackBytes();

        /**
         * @brief Publish a module this embedder loaded itself, under the ruled
         *        identity, and root it in this space.  Returns the published
         *        module — which is the one already published under `id` if there
         *        is one, so two importers of the same identity share a module.
         *
         * For an embedder that calls a ModuleProvider directly instead of going
         * through getImportModule.  Before P3 such a load reached neither
         * SharedModuleCache nor any moduleRoots, and its only anchor was inside
         * the providing runtime — so destroying that runtime dropped it.
         */
        const ProtoObject* registerModule(const ModuleIdentity& id, const ProtoObject* module);

        /** @brief The module published under `id`, or nullptr. */
        static const ProtoObject* findModule(const ModuleIdentity& id);

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

        std::atomic<proto_ulong> nextMutableRef;

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
            proto_ulong count;
            FreeChunk* next;
        };
        FreeChunk* freeChunks;
        FreeChunk* freeChunkPool;
        static constexpr proto_ulong CELL_CHUNK_SIZE = 8192;

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
         * cycle's mark phase.  Published by the GC thread at end of cycle.
         * Under the adaptive heap controller it is the live set L of the
         * out-of-memory rule (since 2.14.4); with fixed limits it only
         * appears in the abort's diagnostic message.
         */
        std::atomic<proto_ulong> liveCellsLastCycle;

        /**
         * @brief Cells reclaimed (swept into the freelist) by the most
         * recently completed GC cycle.  Published by the GC thread at end of
         * cycle.
         *
         * This is the authoritative out-of-memory signal for the fixed-limit
         * path (ProtoSpace::waitForHeapHeadroom), and the no-progress backstop
         * under the adaptive heap controller: once the heap is at its
         * ceiling, two consecutive completed cycles that each reclaim zero
         * cells mean no collection can free space — genuine, unrecoverable
         * OOM.  Reclamation, not mark-phase reachability, is the metric:
         * retained-but-unmarked cells (a live context's un-submitted young
         * generation) fill the heap without ever entering `markedList`, so a
         * mark-based count would under-report and miss that OOM.
         */
        std::atomic<proto_ulong> reclaimedLastCycle;

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

        // P3: RETIRED.  The module list is the process-global ModuleRootTable
        // (core/ModuleRoots.cpp); removable embedder pins belong in a
        // ProtoRootSet (see createRootSet above).  These two fields are retained
        // so the ProtoSpace layout does not change — the same treatment
        // `tupleRoot` and `stringInternMap` already get — and are held empty and
        // never iterated.  Do not add to them: call addModuleRoot() for a module,
        // or createRootSet() for anything that must be unpinned.
        //
        // Until 2.2.0 GC Phase 2 iterated `moduleRoots` INSIDE the stop-the-world
        // window, holding `moduleRootsMutex` there, O(modules).  That was the one
        // term in the documented pause profile that scaled with the program, and
        // it was missing from the cost table in docs/GarbageCollector.md.
        std::vector<const ProtoObject*> moduleRoots;
        std::mutex moduleRootsMutex;

        /** @brief Global reentrant mutex for protecting space-wide metadata (interning, thread registry, GC state). */
        PROTOCORE_DATA static std::recursive_mutex globalMutex;

        // --- Embedder root sets (see `createRootSet`) ---
        std::vector<ProtoRootSet*> rootSets_;
        mutable std::mutex rootSetsMutex_;

        // --- Errors (see setErrorFactory, ProtoException) ---
        std::atomic<ProtoErrorFactory> errorFactory_{nullptr};
        // Pins of in-flight ProtoExceptions; created with the space.
        ProtoRootSet* exceptionRoots_{nullptr};
        // protoCore's error prototypes: [0, kProtoErrorKindCount) per kind,
        // [kProtoErrorKindCount] the root.  Built once, pinned in errorRoots_.
        std::once_flag errorPrototypesOnce_;
        const ProtoObject* errorPrototypes_[kProtoErrorKindCount + 1]{};
        ProtoRootSet* errorRoots_{nullptr};

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
        std::vector<proto_ulong> gcFinalizedMutableRefs;
    };
}

#endif /* PROTO_H_ */
