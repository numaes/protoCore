# Architecture Deep Dive: The Object and Type System

## Introduction

Proto features a dynamic and flexible object model that differs significantly from the class-based systems found in languages like C++ or Java. It combines a prototype-based inheritance system with an efficient memory representation using tagged pointers, creating a powerful foundation for dynamic languages.

## Tagged Pointers

In a typical dynamic language, every value, no matter how small, is an object that must be allocated on the heap. This can lead to significant overhead. Proto avoids this by using **tagged pointers**.

A `ProtoObjectPointer` is a single machine word (e.g., 64 bits) that can represent either a true pointer to a heap-allocated object or an immediate value. This is achieved by using the lower few bits of the word as a "tag" to indicate the type of data being stored.

Because heap-allocated objects are always aligned to a certain byte boundary (e.g., 8 or 16 bytes), their memory addresses will always have their lowest bits as zero. We can take advantage of this to store information.

*   If the lowest bit is `0`, the value is a true pointer to a `ProtoObject` on the heap.
*   If the lowest bit is `1`, the value is an immediate 63-bit signed integer.
*   Other tag patterns can be used for booleans, null, or other special singletons.

The `ProtoObjectPointer` union from `proto_internal.h` shows this concept in practice:

```cpp
// Illustrative snippet from proto_internal.h
union ProtoObjectPointer {
    // The raw 64-bit value
    uint64_t raw;

    // The value interpreted as a pointer to a heap object
    ProtoObject* as_object;

    // The value interpreted as a signed integer
    int64_t as_integer;
};
```

### Benefits

The primary benefit is **performance**. Creating or passing around an integer does not require any heap allocation or garbage collection overhead. This makes numerical and logical operations significantly faster.

## Prototype-Based Inheritance

Instead of classes, Proto uses prototypes. Every object can be a prototype for another object. When you try to access an attribute or method on an object, the runtime first looks at the object itself. If the attribute isn't found, it follows the object's `parent` link and looks at its prototype. This process continues up the chain until the attribute is found or the chain ends.

This is different from classical inheritance, where a class defines a rigid structure that all its instances must follow. The prototype model is more flexible, allowing objects to have their structures and behaviors modified dynamically at runtime.

## Immutable Data Structures

To ensure thread safety and enable efficient sharing, Proto's core collection types, such as `ProtoString` and `ProtoTuple`, are **immutable**. Once created, their contents cannot be changed.

This might sound inefficient, but these types are implemented using sophisticated tree-like data structures (similar to ropes for strings or AVLs for tuples). This has a powerful consequence: operations that would normally be expensive are very cheap.

For example, concatenating two large strings does not involve copying all the characters. Instead, it creates a new, small "node" object that simply holds pointers to the two original strings. This makes operations like concatenation, slicing, or appending an O(log N) operation instead of O(N), as no full copies are ever needed. This design is crucial for building scalable and performant systems that heavily manipulate data.
