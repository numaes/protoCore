=========================================
Conceptual Introduction to Proto
=========================================

Proto is more than a runtime; it is a complete, high-performance object system designed from first principles. It brings the dynamism and flexibility of prototype-based languages into the C++ ecosystem, guided by a philosophy of safety through immutability and performance through a highly optimized, concurrent memory model. Understanding its core architectural pillars is key to leveraging its full potential.

The primary public interface is exposed via ``proto.h``.

Architectural Pillar 1: The Immutable-First Object Model
--------------------------------------------------------

**ProtoObject: The Universal, Dynamic Base**

Every entity in Proto is a ``ProtoObject``. It is the polymorphic base class for all data, from a simple integer to a complex, user-defined object. This provides a consistent and powerful API for interacting with any piece of data in the system.

Unlike traditional C++ class hierarchies, Proto's object model is based on **prototypes**. Objects are not instances of a rigid class; they are cloned from other objects and can have their structure and behavior modified dynamically at runtime.

*   :cpp:func:`proto::ProtoObject::newChild` creates a new object that inherits from the original.
*   :cpp:func:`proto::ProtoObject::getAttribute` and :cpp:func:`proto::ProtoObject::setAttribute` allow for dynamic property access.

**ParentLink: Enabling Multiple Inheritance**

The ``ParentLink`` is the mechanism that enables Proto's powerful prototype chain. Each object can have multiple parents, forming a directed acyclic graph. When an attribute is accessed on an object, the runtime first checks the object itself. If the attribute is not found, it traverses the ``ParentLink`` chain until the attribute is found or the chain is exhausted. This enables flexible and powerful code and data sharing.

**Immutable Data Structures: Concurrency by Design**

To eliminate a vast class of concurrency bugs, Proto's core data structures are **immutable by default**.

*   :cpp:class:`proto::ProtoTuple`: An immutable, ordered sequence of objects. Any operation that "modifies" a tuple, such as adding an element, returns a *new* tuple, efficiently sharing memory with the original for any unchanged parts (structural sharing).
*   :cpp:class:`proto::ProtoString`: An immutable sequence of characters, built upon the same efficient, rope-like implementation as ``ProtoTuple``.
*   :cpp:class:`proto::ProtoList`: For cases where mutability is required, the ``ProtoList`` provides a mutable, ordered collection.

This immutable-first design is the cornerstone of Proto's elite concurrency model. It makes parallel programming fundamentally safer and easier to reason about, as it removes the need for complex locking around shared data.

Architectural Pillar 2: The High-Performance Memory Model
---------------------------------------------------------

**Tagged Pointers: Avoiding Allocation for Primitives**

Proto employs a critical optimization known as **tagged pointers**. For simple, common data types like integers, booleans, and even dates, the value is stored directly within the 64-bit pointer itself, using tag bits to differentiate it from an actual memory address. This completely avoids heap allocation and garbage collector overhead for these types, providing a significant performance boost.

**Cell & BigCell: The Atomic Unit of Memory**

For any object that requires heap allocation, the fundamental unit of memory is the ``Cell``. All cells are a fixed size (64 bytes, aliased as ``BigCell``). This design has several advantages:

*   **Extremely Fast Allocation**: Allocating memory is as simple as taking a free cell from a list.
*   **No Fragmentation**: Fixed-size blocks eliminate memory fragmentation.
*   **Efficient GC**: The garbage collector can treat all memory uniformly.

**Low-Latency, Concurrent Garbage Collector**

Proto features a custom, concurrent, stop-the-world garbage collector designed for minimal application pauses. It runs in its own thread, and the "stop-the-world" phase—where application threads are paused—is extremely short, typically only for scanning the root set of objects. This makes Proto suitable for interactive and soft real-time applications.

Architectural Pillar 3: The Concurrent Execution Environment
----------------------------------------------------------

**ProtoSpace: The Isolated Universe**

A ``ProtoSpace`` is a self-contained runtime environment. It manages its own heap, its own garbage collector, and its own set of threads. Multiple ``ProtoSpace`` instances can exist within the same process, providing a powerful sandboxing mechanism for plugins or multi-tenant applications. They are completely isolated from one another, yet can share immutable data like interned strings and tuples, maximizing memory efficiency.

**ProtoThread: True, GIL-Free Concurrency**

Each ``ProtoThread`` is a true, native OS thread. Proto was designed from the ground up for multithreading and has no Global Interpreter Lock (GIL). The internal implementation of a thread (`ProtoThreadImplementation`) is carefully structured to fit within the 64-byte `Cell` size, using an extension cell (`ProtoThreadExtension`) for larger data members, ensuring it integrates perfectly with the memory manager.

**ProtoContext: The Execution State**

The ``ProtoContext`` is an essential object that is passed to nearly every function. It represents the current state of a thread's execution, including:

*   The call stack (via a link to the `previous` context).
*   The local variables for the current scope.
*   A link to the current ``ProtoThread`` and ``ProtoSpace``.

Crucially, the ``ProtoContext`` also acts as the **primary factory for creating objects**. Calling methods like :cpp:func:`proto::ProtoContext::fromInteger` or :cpp:func:`proto::ProtoContext::newList` ensures that newly created objects are correctly registered with the memory manager and garbage collector.
