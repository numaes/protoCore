# Architecture Deep Dive: The Object Model and protoContext

## Introduction

At the heart of ProtoCore lies a highly dynamic, structurally flexible object model designed to provide maximum expressive power with minimal runtime overhead. Eschewing the rigid, static structures of class-based inheritance (as seen in C++ or Java), Proto implements a sophisticated prototype-based system backed by highly optimized memory representations and a thread-local execution anchor known as the `protoContext`.

## The `protoContext` Life Cycle

The `protoContext` is the foundational execution environment for any thread interacting with the ProtoCore runtime. It acts as the vital bridge between a localized OS thread and the global, shared memory space (`ProtoSpace`).

### Initialization and Thread Binding
When a new execution thread is spawned (or an existing C++ thread attaches to the runtime), a new `protoContext` must be instantiated. 
1.  **Allocation:** The context is allocated and strictly bound to the thread via Thread-Local Storage (TLS). This ensures that context lookups are instantaneous and do not require cross-thread synchronization.
2.  **Space Attachment:** The context is firmly registered with the global `ProtoSpace`, allowing the Garbage Collector to track its lifecycle and monitor its local stack for GC root scanning.

### The Role of the Context
The `protoContext` is ubiquitous in the Proto API; virtually all operations require it as the first argument. It serves several critical, high-performance roles:
*   **Allocation Anchor:** It holds thread-local allocation buffers, allowing the thread to rapidly allocate new Cells without acquiring global heap locks.
*   **Execution State:** It tracks the current call frame, execution depth, and handles exception unwinding.
*   **The Attribute Cache:** It houses the thread-local cache used to accelerate prototype resolution (detailed below).

### Destruction and Teardown
When a thread finishes execution, the `protoContext` undergoes a rigorous teardown sequence. It flushes any pending allocations to the global pools, clears its thread-local caches, and finally detaches from the `ProtoSpace`. This signals the Garbage Collector that the thread's stack no longer needs to be scanned during the Stop-The-World phase.

## Tagged Pointers: Eliminating Heap Overhead

In fully dynamic languages, every primitive value (integers, booleans, short strings) conceptually behaves as an object. If every such value required a full heap allocation, memory fragmentation and GC pressure would paralyze the system.

ProtoCore circumvents this entirely utilizing **Tagged Pointers**.

A `ProtoObject` handle is represented by a single 64-bit machine word. Rather than always acting as a raw memory address, Proto utilizes the lowest bits of this word—which are guaranteed to be zero for 64-byte aligned heap allocations—as a type "tag".

This encoding scheme allows the runtime to instantly classify the handle:
*   **Heap Object (Tag `00`):** The remaining 62 bits represent a direct memory address to an immutable 64-byte Cell on the GC heap.
*   **SmallInt (Tag `10`):** The upper bits store a direct, 54-bit signed integer. 
*   **Inline String (Tag `01` / `100`):** The handle embeds up to 6 bytes of UTF-8 string data directly within the pointer itself.
*   **Symbol (Tag `10110`):** Represents an interned string or attribute key, enabling $O(1)$ identity comparisons.

### Performance Impact
By packing immediate values directly into the pointer, Proto eliminates millions of unnecessary heap allocations. Mathematical operations on `SmallInts` occur instantly in registers, entirely bypassing the memory allocator and the Garbage Collector.

## Prototype-Based Inheritance

Proto employs prototype delegation rather than classical inheritance. Every object implicitly holds a `parent` link to another object (its prototype).

When a program attempts to read an attribute (e.g., `object.method()`), the runtime queries the object's internal state. If the attribute is absent, the runtime transparently traverses the `parent` link, querying the prototype. This delegation continues up the prototype chain until the attribute is found or the chain terminates.

This provides extreme flexibility, allowing developers to dynamically construct and modify inheritance hierarchies at runtime, creating patterns impossible in rigid, class-based architectures.

### The Thread-Local Attribute Cache

The inherent risk of prototype delegation is the cost of walking the prototype chain on every attribute access. To mitigate this, Proto implements a highly aggressive caching strategy anchored within the `protoContext`.

Every `protoContext` maintains a thread-local, 1024-entry **Attribute Cache**.
1.  When an attribute is accessed, the context computes a rapid bitwise hash of the `{Object Identity, Attribute Symbol}` pair.
2.  This hash indexes the local cache array.
3.  On a cache hit, the fully resolved value (even if it was inherited deep in the prototype chain) is returned in $O(1)$ time, typically taking less than 10 nanoseconds.

Because this cache is strictly thread-local to the `protoContext`, it requires zero atomic operations or mutex locks to access, resulting in elite execution speeds for polymorphic property access.
