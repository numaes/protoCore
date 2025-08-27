# Architecture Deep Dive: The Foreign Function Interface (FFI)

## Introduction

No system is an island. For a runtime like Proto to be useful in the real world, it must be able to integrate seamlessly with existing C and C++ libraries. The Foreign Function Interface (FFI) is the mechanism that allows code running inside Proto to call native C/C++ functions and for native code to interact with Proto objects.

## Proto's FFI Advantage: No Memory Compaction

The single biggest challenge when building an FFI for a garbage-collected language is dealing with memory movement. In many GCs, objects can be moved in memory during a compaction phase. This means native C++ code cannot safely hold a direct pointer to an object, as it could be invalidated at any moment. This usually requires a complex and slow "handle" system.

Proto's GC **does not move or compact memory**. This provides a massive advantage for FFI integration. Since an object's address is stable for its entire lifetime, C++ code can hold direct pointers to Proto objects. This makes the FFI simpler, faster, and more natural to work with.

## The Safe FFI Pattern

### The Challenge

While pointers are stable, we still need to coordinate with the garbage collector. If you call a long-running native function, the GC's background thread might run while the native code is executing. The GC needs to know that the Proto objects passed to the native function are still alive, but it cannot scan the native function's stack.

### The Solution: The `setManaged()` / `setUnmanaged()` Protocol

Proto solves this with a simple, explicit protocol. Before calling a native function that might take a long time to execute, the thread must tell the GC that it is entering an "unmanaged" state. When the native function returns, the thread must tell the GC it is back in a "managed" state.

*   `setUnmanaged()`: Signals to the GC that this thread's stack is now opaque and should not be scanned for roots. The thread is temporarily detached from the GC's control.
*   `setManaged()`: Signals that the thread is back under GC control. This will trigger a GC cycle if one was requested while the thread was unmanaged.

This protocol ensures that the GC will not run while native code is executing, preventing any possibility of an object being collected while it's still being used by the C++ code.

### RAII Guard for Safety

Manually calling `setManaged()` and `setUnmanaged()` can be error-prone, especially in the face of C++ exceptions. The best practice is to wrap the FFI call in a simple RAII (Resource Acquisition Is Initialization) guard class.

Here is an example of what such a guard might look like:

```cpp
// Illustrative FFIGuard class
class FFIGuard {
public:
    FFIGuard(Thread* thread) : p_thread(thread) {
        p_thread->setUnmanaged();
    }

    ~FFIGuard() {
        p_thread->setManaged();
    }

private:
    Thread* p_thread;
};

// Usage:
void my_native_wrapper(Thread* thread, ProtoObject* arg) {
    FFIGuard guard(thread); // Enters unmanaged mode

    // Now it's safe to call long-running C/C++ code
    native_library_function(arg->as_string()->c_str());

} // guard goes out of scope here, automatically re-enters managed mode
```

## Marshalling Data

"Marshalling" is the process of converting data between the Proto world and the native C++ world. This typically involves creating a C++ wrapper function that handles the conversion.

Here is a simple example of a wrapper that exposes a C function `count_characters` to the Proto runtime.

```cpp
// A native C function we want to call
int count_characters(const char* str) {
    return strlen(str);
}

// The C++ wrapper function exposed to Proto
ProtoObject* proto_count_characters(Thread* thread, ProtoObject* p_str) {
    // 1. Use a guard for GC safety
    FFIGuard guard(thread);

    // 2. Marshall the arguments from Proto to C++
    // This includes type checking.
    if (!p_str->is_string()) {
        // In a real implementation, you would throw a Proto exception here
        return thread->runtime->new_nil();
    }
    const char* native_str = p_str->as_string()->c_str();

    // 3. Call the actual native function
    int result = count_characters(native_str);

    // 4. Marshall the return value from C++ back to Proto
    return thread->runtime->from_integer(result);
}
```
