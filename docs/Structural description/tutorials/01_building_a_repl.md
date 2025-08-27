# Tutorial: Building a REPL from Scratch

## Introduction

A REPL (Read-Eval-Print-Loop) is a simple, interactive command-line interface for a language. It's one of the best ways to learn a new runtime, as it gives you immediate feedback. The `proto` executable you built in the Quick Start guide is a REPL.

In this tutorial, we will build our own minimal REPL from scratch in C++. This will teach you the fundamentals of embedding the Proto runtime in a host application.

## The Core Loop

The logic of a REPL is right in its name:

1.  **Read:** Read a line of input from the user.
2.  **Eval:** Evaluate that line of code.
3.  **Print:** Print the result of the evaluation.
4.  **Loop:** Repeat.

We can represent this with a simple `while` loop in C++.

## Step 1: Initialization

First, we need to include the main Proto header and create a `ProtoContext`. The context is our handle to the entire Proto runtime.

```cpp
#include "proto.h"
#include <iostream>
#include <string>

int main() {
    ProtoContext* context = new ProtoContext();
    std::cout << "Welcome to the Mini Proto REPL!\n";
    // ... REPL loop will go here ...
    delete context;
    return 0;
}
```

## Step 2: The Loop (Read, Eval, Print)

Now let's add the core loop inside our `main` function.

```cpp
// Inside main(), after creating the context
std::string line;
while (true) {
    // 1. READ
    std::cout << "> ";
    if (!std::getline(std::cin, line) || line == "exit") {
        break;
    }

    // 2. EVAL
    // For this simple REPL, we'll lean on the proto_python
    // library's ability to evaluate a single line of code.
    // In a real application, this might involve a more complex parser.
    ProtoObject* result = context->evalPython(line);

    // 3. PRINT
    if (result) {
        std::cout << result->toString() << std::endl;
    }
}
```

### A Note on Evaluation

The `context->evalPython(line)` function is a high-level utility provided for convenience. It takes a string of Python code, transpiles it to Proto-compatible C++ in memory, executes it, and returns the result as a `ProtoObject*`. This is perfect for a simple tool like this.

For a more advanced language, the "Eval" step would involve parsing the input text into an Abstract Syntax Tree (AST) and then walking that tree to execute the logic using Proto's C++ API.

## Step 3: Putting It All Together

Here is the complete `main.cpp` for our minimal REPL.

**`repl.cpp`:**
```cpp
#include "proto.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    // 1. Initialize the runtime
    ProtoContext* context = new ProtoContext();

    std::cout << "Welcome to the Mini Proto REPL! Type 'exit' to quit.\n";

    std::string line;
    while (true) {
        // 2. READ a line of input
        std::cout << "> ";
        if (!std::getline(std::cin, line) || line == "exit") {
            break;
        }

        if (line.empty()) {
            continue;
        }

        // 3. EVALUATE the line using the Python bridge
        ProtoObject* result = context->evalPython(line);

        // 4. PRINT the string representation of the result
        if (result) {
            std::cout << result->toString() << std::endl;
        }
    }

    std::cout << "Goodbye!\n";
    delete context;
    return 0;
}
```

To build this, you would link it against `libproto.a` just like in the `building_on_proto` guide.

## Next Steps

This REPL is very basic. Here are some features you could add to improve it:

*   **Better Error Handling:** The `evalPython` function can return an error object. Check for it and print a helpful error message.
*   **Multi-line Input:** Allow users to enter code that spans multiple lines.
*   **History:** Use a library like `readline` to allow users to press the up and down arrows to access previous commands.
