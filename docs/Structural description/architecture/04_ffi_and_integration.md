# Architecture Deep Dive: Foreign Function Interface (FFI) and Critical Sections

## Introduction

A modern runtime cannot exist in isolation; it must interoperate seamlessly with the massive ecosystem of existing native libraries. The Foreign Function Interface (FFI) is the bridge that permits code executing within the ProtoCore engine to transparently invoke native C/C++ functions, and conversely, allows native code to manipulate Proto objects.

Developing an FFI within a garbage-collected environment is notoriously hazardous. The primary architectural challenge is ensuring that the Garbage Collector does not reclaim objects that are actively being used by native C++ code, which the GC cannot inherently monitor.

## The ProtoCore Advantage: Immutability and Stable Addresses

In highly compacting garbage collectors (like those frequently utilized in Java or V8), the physical memory location of an object is volatile. The GC routinely moves objects in memory to prevent heap fragmentation. Consequently, native C++ code cannot safely hold a raw memory pointer to an object; if a GC cycle occurs during a native function call, the pointer becomes instantly invalid, leading to catastrophic memory corruption (segfaults). Traditional VMs solve this by forcing native code to use cumbersome, indirect "Handles," which severely degrades FFI performance.

ProtoCore's GC **does not compact memory**. Every object is allocated as an immutable 64-byte Cell that remains firmly anchored at its original memory address until it is explicitly proven dead and collected. 

This architectural decision yields a massive FFI advantage: **C++ code can safely, directly hold raw pointers to Proto objects.** This eliminates the overhead of Handle indirection, making Proto's FFI inherently faster, safer, and significantly more ergonomic for systems programmers.

## Managing the GC Lifecycle: FFI Critical Sections

While memory addresses in Proto are stable, the lifecycle of those objects is still managed by the concurrent GC. If a user thread invokes a computationally expensive, long-running native C++ function (e.g., a complex matrix multiplication or a blocking network request), a critical synchronization problem arises.

If the background Garbage Collector attempts to initiate its Stop-The-World (STW) Root Scan phase, it must wait for all active user threads to yield cooperatively at a **Critical Section** boundary. However, a thread executing deep inside external, native C++ code cannot poll the GC's yield flag. If left unmanaged, the long-running native call would effectively deadlock the entire Garbage Collector, causing memory to rapidly exhaust.

### The Solution: Explicit Thread Unmanagement

To solve this, Proto mandates a precise, highly efficient protocol for crossing the FFI boundary: `setUnmanaged()` and `setManaged()`.

Before a thread steps across the FFI boundary into an arbitrary native function, it must signal the `ProtoSpace` that it is temporarily relinquishing its execution managed status.

*   `context->setUnmanaged()`: The thread declares that it is entering native code. Crucially, the thread guarantees that **its current C++ stack contains no live Proto roots beyond the point of this call**, and that it will not interact with the Proto API while unmanaged.
    *   *GC Interaction:* When a thread is marked "unmanaged," the Garbage Collector considers that thread effectively paused. If a GC cycle triggers, the GC will **not** wait for this thread to yield. It will proceed with the Root Scan, safely ignoring the unmanaged thread.
*   `context->setManaged()`: The thread has returned from native code and is re-entering the Proto runtime.
    *   *GC Interaction:* This is a **Critical Section**. Before the thread is allowed to continue executing Proto logic, it checks the global GC status. If the background GC is currently executing a Stop-The-World phase, the `setManaged()` call will block, safely parking the thread until the GC STW phase concludes.

### Ensuring Safety via RAII

Manually tracking unmanaged state boundaries is error-prone and highly susceptible to C++ exception leaks. Proto strongly advocates utilizing RAII (Resource Acquisition Is Initialization) to enforce boundary safety.

```cpp
// A scoped guard to guarantee FFI GC safety
class FFIGuard {
public:
    explicit FFIGuard(ProtoContext* ctx) : p_context(ctx) {
        // Leave the managed runtime
        p_context->setUnmanaged();
    }

    ~FFIGuard() {
        // Safely re-enter the managed runtime (Yielding if GC STW is active)
        p_context->setManaged();
    }

private:
    ProtoContext* p_context;
};
```

## Marshalling and Execution

With the GC safety protocol established, invoking a native function simply becomes an exercise in parameter marshalling—converting Proto types to C++ types and vice versa.

```cpp
// 1. The native library function we wish to integrate
int native_string_length(const char* str) {
    return std::strlen(str); // Example long-running operation
}

// 2. The FFI Wrapper bound to ProtoCore
const ProtoObject ffi_string_length(ProtoContext* context, const ProtoObject p_str) {
    // A. Validate types before leaving managed code
    if (!p_str->is_string()) {
        return context->space->new_nil();
    }

    // Extract the raw C-string. (Safe because Proto strings are immutable and addresses are stable)
    const char* raw_str = p_str->as_string()->c_str();
    int result_length = 0;

    {
        // B. Establish the Critical Section boundary
        FFIGuard guard(context); 
        
        // C. Execute native code. The GC may freely run in the background while this executes.
        result_length = native_string_length(raw_str);
    } 
    // D. The guard destructor automatically calls setManaged(), yielding to the GC if necessary.

    // E. Marshall the native result back into a Proto Object
    return context->space->from_integer(result_length);
}
```

By adhering to this lightweight, explicit unmanaged protocol, developers can seamlessly integrate massively complex, blocking C++ libraries without ever compromising the predictable, low-latency guarantees of the ProtoCore Garbage Collector.
