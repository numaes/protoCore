# Guide: Building Your Own Systems with Proto

## Introduction

Proto is not a standalone language; it's a library designed to be the powerful engine inside other systems. This guide outlines the two primary ways you can integrate Proto to build your own high-performance applications, virtual machines, or language runtimes.

## The Two Integration Paths

There are two main approaches to building on top of Proto, each suited for different needs.

1.  **The `proto_python` Transpiler (The Easy Path):** This is the quickest way to leverage Proto's performance. You write code in a subset of Python, and the `proto_python` tool automatically converts it into highly optimized C++ code that uses the Proto runtime. This is ideal for application development where you want the productivity of Python with the performance of a compiled backend.

2.  **Direct C++ Integration (The Power Path):** This is for developers who want to build core systems, like a new language VM, a JIT compiler, or a high-performance game engine component, directly in C++. This path gives you maximum control and performance.

### Case Study: `protoDB`

The `protoDB` project is a perfect real-world example of the transpiler path. It's a complex object database written entirely in Python, but it uses `proto_python` to transpile its codebase into a native executable. This allows it to achieve performance that would be impossible with a standard Python interpreter, demonstrating the power of the transpiler approach.

## Direct C++ Integration Workflow

When you need to build a system directly in C++, the workflow is straightforward.

1.  **Include `proto.h`:** This is the main public header file that exposes all of Proto's core functionality.

2.  **Create a `ProtoContext`:** Your C++ application will create and hold an instance of a `ProtoContext`. This object is your primary interface to the runtime, used for creating objects, calling functions, and managing memory.

3.  **Compile and Link against `libproto.a`:** Your C++ code is compiled as usual, and you simply link the final executable against the `libproto.a` static library that you built in the Quick Start guide.

### Example: A Simple C++ Host Application

Here is a minimal `main.cpp` that demonstrates how to initialize the runtime, create a few objects, and interact with them.

**`main.cpp`:**
```cpp
#include "proto.h"
#include <iostream>

int main() {
    // 1. Create a context, which initializes the runtime
    ProtoContext* context = new ProtoContext();

    // 2. Create some Proto objects
    ProtoObject* my_int = context->fromInteger(42);
    ProtoObject* my_str = context->newString("Hello from C++!");

    // 3. Interact with the objects (here, just printing)
    // In a real app, you would call functions, pass them to other objects, etc.
    std::cout << "Integer value: " << my_int->asInteger() << std::endl;
    std::cout << "String value: " << my_str->asString()->c_str() << std::endl;

    // 4. Clean up the context
    delete context;
    return 0;
}
```

**`CMakeLists.txt`:**
To build this, you would use a simple `CMakeLists.txt` file.

```cmake
cproject(MyProtoApp CXX)

# Find the Proto library (assuming it was installed or is in a known path)
find_library(PROTO_LIBRARY NAMES proto PATHS /path/to/proto/build)

add_executable(my_app main.cpp)

# Link your application against libproto.a
target_link_libraries(my_app ${PROTO_LIBRARY})
```

### Key Concepts for Direct Integration

*   **Object Creation:** Use methods on the `context` object, like `fromInteger()`, `newString()`, `newTuple()`, etc., to create new Proto values.
*   **Calling Proto Functions:** To call a function within the Proto world, you would first look up the function object (e.g., from another object's attributes) and then use `context->call(function, args)`.
*   **Error Handling:** Operations within Proto can fail (e.g., type errors, invalid arguments). These failures typically return a special error object. Your C++ host code should check for these error objects and handle them appropriately.
