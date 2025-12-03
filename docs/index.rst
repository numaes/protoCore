.. Proto documentation master file.

Welcome to the Proto Documentation
====================================

Proto is a powerful, embeddable runtime written in modern C++ that brings the flexibility of dynamic, prototype-based object systems into the world of high-performance, compiled applications.

This documentation provides a complete reference to the Proto public API, along with an overview of the core design principles that guide its architecture.

.. note::

   This project is in an active development phase. The API is stable, but we welcome feedback and contributions to improve its design and performance.

A Note on API Design: Immutability and `const`
------------------------------------------------

A fundamental principle of Proto is **immutability**. All data structures, such as lists, strings, and objects, are immutable by default.

What this means for you as a developer:

*   **Operations that "modify" an object do not change it.** Instead, they return a **new** version of the object with the changes applied.
*   The original object remains untouched, making your code safer, especially in concurrent environments.

This design is reflected directly in the API through **const-correctness**. You will notice that most methods are `const` and return `const` pointers (e.g., `const ProtoList*`). This is a deliberate design choice that the compiler enforces, guiding you to write safer, more predictable code.

.. code-block:: cpp

   // WRONG: This will not compile, as 'list' is a const pointer.
   // const ProtoList* list = context->newList();
   // list->some_modifying_method(); 

   // RIGHT: Capture the new version returned by the method.
   const ProtoList* list = context->newList();
   const ProtoList* modified_list = list->appendLast(context, value);

   // 'list' is unchanged, 'modified_list' contains the new value.
   ASSERT_NE(list, modified_list);


Table of Contents
-----------------

.. toctree::
   :maxdepth: 2
   :caption: Getting Started

   project_overview
   target_applications

.. toctree::
   :maxdepth: 2
   :caption: Core Concepts

   conceptual_introduction
   numerics
   execution_model
   DESIGN_link

.. toctree::
   :maxdepth: 1
   :caption: API Reference

   api/library_root

.. toctree::
   :maxdepth: 1
   :caption: Project

   commercial_potential


.. note::
   The `DESIGN.md` file provides a much deeper dive into the technical architecture, including the memory model, garbage collector, and concurrency strategy. It is highly recommended for anyone looking to contribute to the project.

Indices and tables
==================

* :ref:`genindex`
* :ref:`search`
