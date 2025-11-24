# Proto Documentation

This directory contains the source files for the Proto project's official documentation.

The documentation is built using **Sphinx**, a powerful documentation generator, and **Doxygen**, which is used to generate API references directly from the C++ source code comments.

## Prerequisites

Before you can build the documentation, you need to ensure the following tools are installed on your system:

*   **Python** (3.6+)
*   **Sphinx**: The primary documentation generator.
*   **Breathe**: A Sphinx extension that acts as a bridge to Doxygen.
*   **Doxygen**: The tool used to extract documentation from the C++ source files.

You can install the required Python packages using pip:

```bash
pip install sphinx breathe
```

To install Doxygen, use your system's package manager. For example:

**On Debian/Ubuntu:**
```bash
sudo apt-get install doxygen
```

**On macOS (using Homebrew):**
```bash
brew install doxygen
```

## Building the Documentation

The build process is managed by CMake, which will first run Doxygen and then Sphinx.

1.  **Configure the project** (if you haven't already) from the root directory:
    ```bash
    mkdir build
    cd build
    cmake ..
    ```

2.  **Build the `docs` target** from within the `build` directory:
    ```bash
    make docs
    ```
    This command will first run Doxygen to generate XML files from the C++ source code and then run Sphinx to convert the `.rst` source files and the Doxygen output into a polished HTML website.

## Viewing the Documentation

After the build process is complete, the generated HTML files will be located in the `build/docs/html` directory.

You can open the main page by opening the following file in your web browser:

```
build/docs/html/index.html
```
