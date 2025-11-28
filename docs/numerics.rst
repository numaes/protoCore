The Numeric System
====================

Proto features a sophisticated and highly efficient numeric system designed to provide both maximum performance for common integer values and the power of arbitrary-precision arithmetic when needed. This is handled transparently through a unified API.

The Core Numeric Types
----------------------

The numeric hierarchy is composed of three core types:

1.  **SmallInteger**: For maximum efficiency, integers that fit within a 56-bit signed range are stored directly inside the 64-bit ``ProtoObject*`` pointer. This technique, known as "tagged pointers," avoids heap allocation entirely for the vast majority of numbers used in typical applications, leading to significant performance gains.

2.  **LargeInteger**: When an integer value exceeds the 56-bit range, Proto automatically and transparently promotes it to a ``LargeInteger``. This is a heap-allocated object that can represent an integer of any size, limited only by available memory. All arithmetic operations handle ``LargeInteger`` objects correctly, allowing for seamless arbitrary-precision arithmetic.

3.  **Double**: For floating-point arithmetic, Proto provides a ``Double`` type, which stores a 64-bit IEEE 754 double-precision number. This is also a heap-allocated object.

This hybrid system means you don't have to worry about the underlying representation. You can perform calculations, and Proto will automatically use the most efficient storage method.

Creating Numbers
----------------

You can create any number using the factory methods on the ``ProtoContext``.

.. code-block:: cpp

   // Create a SmallInteger (stored as a tagged pointer)
   const ProtoObject* small_int = context->fromLong(12345);

   // Create a LargeInteger (will be allocated on the heap)
   const ProtoObject* large_int = context->fromLong(1LL << 60);

   // Create a Double (will be allocated on the heap)
   const ProtoObject* a_double = context->fromFloat(987.65);

Transparent Arithmetic
----------------------

All arithmetic operations on ``ProtoObject`` handle the different numeric types automatically. If you add an integer and a double, the integer will be transparently promoted to a double to perform the calculation.

.. code-block:: cpp

   const ProtoObject* i = context->fromLong(10);
   const ProtoObject* d = context->fromFloat(2.5);

   // The result is a new Double object with the value 12.5
   const ProtoObject* result = i->add(context, d);

   ASSERT_TRUE(result->isDouble(context));
   ASSERT_DOUBLE_EQ(result->asDouble(context), 12.5);

Automatic Transitions
---------------------

The system automatically handles transitions between ``SmallInteger`` and ``LargeInteger`` as results cross the 56-bit boundary.

.. code-block:: cpp

   // The maximum value for a SmallInteger
   long long max_small_int = (1LL << 55) - 1;

   const ProtoObject* a = context->fromLong(max_small_int);
   const ProtoObject* b = context->fromLong(1);

   // The addition overflows the SmallInteger range...
   const ProtoObject* large_result = a->add(context, b);

   // ...so Proto automatically returns a LargeInteger.
   ASSERT_TRUE(isLargeInteger(large_result)); 
   ASSERT_EQ(large_result->asLong(context), max_small_int + 1);


   // Conversely, subtracting brings the value back into range...
   const ProtoObject* small_result = large_result->subtract(context, b);

   // ...so Proto returns a SmallInteger.
   ASSERT_TRUE(isSmallInteger(small_result));
   ASSERT_EQ(small_result->asLong(context), max_small_int);

This transparent management makes the numeric system both powerful and easy to use.
