================
Project Overview
================

Proto is a powerful, embeddable runtime written in modern C++ that brings the flexibility of dynamic, prototype-based object systems (like those in JavaScript or Python) into the world of high-performance, compiled applications.

It is designed for developers who need to script complex application behavior, configure systems dynamically, or build domain-specific languages without sacrificing the speed and control of C++. With Proto, you get an elegant API, automatic memory management, and a robust, immutable data model designed for elite concurrency.

Core Features
-------------

*   **Dynamic Typing in C++**: Create and manipulate integers, floats, booleans, strings, and complex objects without compile-time type constraints.

*   **Prototypal Inheritance**: A flexible and powerful object model based on Lieberman prototypes. Objects inherit directly from other objects, allowing for dynamic structure and behavior sharing without the rigidity of classical inheritance.

*   **Immutable-by-Default Data Structures**: Collections like lists, tuples, and dictionaries are immutable. Operations like ``append`` or ``set`` return new, modified versions, eliminating a whole class of bugs related to shared state and making concurrent programming safer and easier to reason about.

*   **Elite Concurrency Model**: By leveraging immutability, Proto provides a foundation for true multi-core scalability, free from the limitations of mechanisms like Python's GIL.

*   **Low-Latency Automatic Memory Management**: A concurrent, stop-the-world garbage collector manages the lifecycle of all objects, freeing you from manual ``new`` and ``delete`` with minimal application pauses.

*   **Clean, Embeddable C++ API**: The entire system is exposed through a clear and minimal public API (`proto.h`), making it easy to integrate into existing C++ applications.
