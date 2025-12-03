
The Execution Model and Garbage Collection
==========================================

Proto's execution model is designed for performance, concurrency, and robust memory management. At its core is the interplay between the ``ProtoContext``, which represents a function's execution frame, and the Garbage Collector (GC). This design enables a form of generational garbage collection that minimizes application pauses.

The ProtoContext: An Activation Frame
-------------------------------------

Each time a function is called within the Proto runtime, a new ``ProtoContext`` is created. This context serves as the **activation frame** for that specific call, holding all the information necessary for its execution:

*   **The Call Stack**: Each context points to its parent (``previous``), forming a linked list that represents the call stack of its thread.
*   **Local Variables**: The context manages two types of local variables, enabling both high-speed access and powerful closure capabilities.
    *   **Automatic Locals**: These are simple, C-style variables stored in a raw array owned by the context. They are extremely fast to access but are destroyed when the function returns. They are suitable for temporary, scratch-space calculations.
    *   **Closure Locals**: These are parameters and other variables that might be "captured" by a nested function (a closure). They are stored in a ``ProtoSparseList``, which is a GC-managed object. This allows them to outlive the function call if a closure still references them.
*   **Argument Binding**: The ``ProtoContext`` constructor is responsible for binding the arguments passed to a function (both positional and keyword) to the declared parameter names, storing them as closure locals. It robustly handles errors like incorrect argument counts or duplicate assignments.

Interaction with the Garbage Collector
-------------------------------------

The interaction between the context and the GC is the key to Proto's efficient memory management. The design implements a **generational collection scheme** where each context acts as a "nursery" or "young generation".

1.  **Allocation in the Nursery**: When code executes within a context, all new objects (like lists, large integers, etc.) are allocated as a simple linked list of cells owned by that context. This allocation is extremely fast, requiring no locks in the common case.

2.  **The "Write Barrier" on Return**: When a function is about to return a value, it sets the ``returnValue`` field on its context.

3.  **Promoting the Survivor**: When the context is destroyed (at the end of a function call), its destructor performs a critical action:
    *   It creates a special ``ReturnReference`` object that points to the ``returnValue``.
    *   This ``ReturnReference`` is allocated **in the parent's context**.
    *   This single action "promotes" the return value, making it part of the parent's object graph and ensuring it survives the collection of the child's objects.

4.  **Submitting the Nursery to the GC**: Immediately after, the destructor calls ``ProtoSpace::submitYoungGeneration()``, handing over the entire linked list of all other objects created within the child context to the main GC.

5.  **Concurrent Marking and Sweeping**:
    *   The GC's "Stop-the-World" (STW) phase is extremely short. It only needs to collect the roots (thread stacks, global objects, and the lists of "young generation" cells submitted by recently-destroyed contexts).
    *   Because Proto's objects are immutable, once the roots are collected, the main GC thread can trace the entire object graph and sweep up unreachable objects **concurrently**, while the application threads continue to run.

This architecture provides the benefits of generational GC—namely, very fast allocation and collection of short-lived objects—while leveraging immutability to perform the most expensive work in parallel, resulting in very low-latency pauses for the application.
