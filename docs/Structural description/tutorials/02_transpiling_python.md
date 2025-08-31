# Tutorial: Transpiling Python with `proto_python`

## Introduction

The `proto_python` project is a powerful tool that acts as a bridge between the high-level, productive world of Python and the high-performance world of the Proto runtime. It is a **transpiler**, not a compiler. This means it reads Python source code and converts it into C++ source code that is specifically written to use the Proto library.

This approach allows you to write application logic in a familiar Python syntax while achieving performance close to native C++. This tutorial will walk you through the process.

## How it Works

The process is a multi-stage pipeline:

1.  **Python Parsing:** `proto_python` first parses your `.py` files into a Python Abstract Syntax Tree (AST).
2.  **AST Transformation:** It then walks this AST and converts Python concepts into their Proto equivalents.
    *   `print()` becomes a call to a Proto `stdout` object.
    *   `x + y` becomes a call to `x.operator_add(y)`.
    *   Function definitions become C++ functions that operate on `const ProtoObject` pointers.
3.  **C++ Code Generation:** Finally, it generates a `.cpp` file containing the transformed logic.
4.  **Compilation:** The `proto_python` tool then invokes your system's C++ compiler (like GCC or Clang) to compile the generated C++ code and link it against `libproto.a`, producing a final, standalone executable.

## A Practical Example

Let's transpile a slightly more complex Python script than "Hello, World!".

**`fib.py`:**
```python
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

result = fib(10)
print(result)
```

### The Transpilation Command

To convert this Python script into a native executable, you use the `proto_python` command-line tool.

```sh
# -o specifies the output executable name
proto_python fib.py -o fib_native
```

This command will perform all the steps described above and, if successful, create a new executable file named `fib_native` in your current directory.

### Running the Native Version

You can now run this executable directly.

```sh
./fib_native
```

You will see the output `55`, but it will have been calculated by the highly optimized Proto runtime, not a Python interpreter.

## Supported Python Subset

`proto_python` does not support the *entire* Python language and its vast standard library. It focuses on a core subset of the language that is well-suited for high-performance, systems-level code. The supported features include:

*   Basic data types: integers, strings, booleans, `None`.
*   Function definitions and calls.
*   `if`/`else` control flow.
*   `while` loops.
*   Basic arithmetic and boolean operators.
*   A small, core set of built-in functions like `print()` and `len()`.

Features that are **not** supported typically involve highly dynamic aspects of Python that do not map well to a compiled, AOT (Ahead-Of-Time) model, such as `eval()`, `exec()`, or dynamic module importing.

## Why Use the Transpiler?

*   **Performance:** It's the easiest way to get a massive performance boost for CPU-bound Python code.
*   **Productivity:** You can still benefit from Python's clean syntax and rapid development cycle for your application logic.
*   **Deployment:** It produces a single, standalone native executable with no dependency on a Python interpreter, simplifying deployment.

The `protoDB` project is the primary example of this workflow. It's a complex application written in Python that relies on `proto_python` to become a high-performance database engine.
