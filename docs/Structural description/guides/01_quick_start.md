# Quick Start: Building and Running Proto

This guide will walk you through the process of downloading, building, and running the Proto library and its command-line interface.

## Prerequisites

Before you begin, make sure you have the following tools installed on your system:

*   **A C++20 Compliant Compiler:** GCC 10+ or Clang 11+
*   **CMake:** Version 3.16 or higher
*   **Git:** For cloning the repository

## Building the Library

The build process uses a standard CMake workflow.

1.  **Clone the Repository:**
    First, clone the Proto source code from GitHub.
    ```sh
    git clone https://github.com/your-repo/proto.git
    cd proto
    ```

2.  **Create a Build Directory:**
    It's good practice to build the project in a separate directory.
    ```sh
    mkdir build
    cd build
    ```

3.  **Configure with CMake:**
    Run CMake to generate the build files for your system.
    ```sh
    cmake ..
    ```

4.  **Compile the Code:**
    Finally, run `make` to compile the library and all associated tools.
    ```sh
    make
    ```

If the build is successful, you will find the compiled `libproto.a` library and the `proto` executable inside the `build/` directory.

## Running the Proto Executable

The `proto` executable is a simple Read-Eval-Print-Loop (REPL) that allows you to interact with the Proto runtime directly. You can run it from the `build` directory:

```sh
./proto
```

You should see a `proto> ` prompt. You can now enter Proto expressions.

## Your First "Hello, Proto!" Program

Let's start with a simple program. The `proto_python` project provides a way to transpile Python code to C++ that uses the Proto runtime. Here is how you would run a simple "Hello, World!":

1.  **Create a Python file named `hello.py`:**
    ```python
    print("Hello, Proto!")
    ```

2.  **Transpile and Run:**
    Use the `proto_python` tool (which you also built) to convert the Python code into a C++ executable that links against Proto.
    ```sh
    # Assuming proto_python executable is in your PATH
    proto_python hello.py -o hello_proto
    ./hello_proto
    ```

    You should see the output:
    ```
    Hello, Proto!
    ```

## Next Steps

Now that you have Proto up and running, you can start exploring its capabilities.

*   **Learn how to build your own applications on top of Proto:** [Building on Proto](./02_building_on_proto.md)
*   **Dive into our tutorials to build a REPL from scratch:** [Building a REPL](../tutorials/01_building_a_repl.md)
*   **Interested in contributing?** [Check out our Contributor's Guide](./03_contributing.md)
