# protoCore Installation Guide

This guide covers installing protoCore on **Linux**, **macOS**, and **Windows**. protoCore is built as a **shared library** (`libprotoCore.so`, `libprotoCore.dylib`, or `protoCore.dll`) and is required by runtimes such as **protoJS** and **protoPython**.

---

## Prerequisites

- **C++20** compiler (GCC 10+, Clang 12+, or MSVC 2019+)
- **CMake** 3.16+
- **Threads** (pthread on Unix; system threading on Windows)

---

## Building from Source (all platforms)

From the protoCore project root:

```bash
cmake -B build -S .
cmake --build build --target protoCore
```

The shared library is produced in `build/`:

- **Linux:** `build/libprotoCore.so`
- **macOS:** `build/libprotoCore.dylib`
- **Windows:** `build/protoCore.dll` (or in a configuration subdirectory, e.g. `build/Release/`)

To run tests after building:

```bash
ctest --test-dir build --output-on-failure
```

---

## Installing the Built Library

After building, you can install the library and header to a prefix (system or staging).

**System install (requires appropriate privileges):**

```bash
sudo cmake --install build --component protoCore
```

**Staging install (e.g. for packaging or local use):**

```bash
cmake --install build --component protoCore --prefix ./dist
```

**Installed layout:**

| File / directory | Default path (Linux/macOS) | Purpose |
|------------------|----------------------------|---------|
| Shared library   | `lib/libprotoCore.so` (or `.dylib` / `.dll`) | Runtime |
| Public header    | `include/protoCore.h` | Compile-time |

On Linux, typical system paths are `/usr/local/lib` and `/usr/local/include` when using the default prefix. Ensure the library is on the linker path (e.g. `LD_LIBRARY_PATH` or `ldconfig`) when building or running applications that depend on protoCore.

---

## Linux

### Option A: Clean TGZ (runtime only, recommended for distribution)

To create a **minimal package** containing only the shared library and public header (no test frameworks):

```bash
cmake -B build -S .
cmake --build build --target protoCore
cmake --build build --target package_protocore_only
```

This produces `build/protoCore-<version>-Linux.tar.gz` with layout:

- `protoCore-<version>-Linux/include/protoCore.h`
- `protoCore-<version>-Linux/lib/libprotoCore.so.<soversion>`

Use this for distribution or embedding; GTest/GMock are not included.

### Option B: Install from package (.deb or .rpm)

Packages are generated with **CPack** after a successful build. Only the **protoCore** component is packed (no test frameworks).

1. **Build and package:**

   ```bash
   cmake -B build -S .
   cmake --build build --target protoCore
   cd build
   cpack -G TGZ
   ```

   For a minimal TGZ you can instead use the `package_protocore_only` target (see Option A).

   This produces, among others:

   - **Debian/Ubuntu:** `protoCore-1.0.0-Linux.deb` (or similar)
   - **Fedora/RHEL:** `protoCore-1.0.0-Linux.rpm` (or similar)

2. **Install:**

   **.deb (Debian/Ubuntu):**
   ```bash
   sudo dpkg -i protoCore-1.0.0-Linux.deb
   ```

   **.rpm (Fedora/RHEL/openSUSE):**
   ```bash
   sudo rpm -ivh protoCore-1.0.0-Linux.rpm
   # or: sudo dnf install protoCore-1.0.0-Linux.rpm
   ```

3. **Verify:** Ensure the library and header are in the expected paths (e.g. `/usr/local/lib`, `/usr/local/include`) and that dependent projects (e.g. protoJS) can find `libprotoCore.so`.

4. **Uninstall:**

   ```bash
   sudo apt remove protoCore    # Debian/Ubuntu
   sudo rpm -e protoCore        # Fedora/RHEL
   ```

### Option C: Build and install from source (Linux)

See **Building from Source** and **Installing the Built Library** above. After install, you may need to run `sudo ldconfig` (Linux) so the dynamic linker finds the library.

---

## macOS

### Option A: Install from package (.dmg or .tgz)

1. **Build and package:**

   ```bash
   cmake -B build -S .
   cmake --build build --target protoCore
   cd build
   cpack
   ```

   CPack generates a **DragNDrop** (.dmg) and/or **TGZ** archive.

2. **Install:** Open the .dmg and drag the package to the desired location, or extract the .tgz and copy the library and header to `/usr/local/lib` and `/usr/local/include` (or another prefix).

3. **Verify:**
   ```bash
   ls /usr/local/lib/libprotoCore.dylib
   otool -L /usr/local/lib/libprotoCore.dylib
   ```

### Option B: Build and install from source

Build as in **Building from Source**, then run:

```bash
sudo cmake --install build --component protoCore
```

Or use a custom prefix (e.g. `--prefix ./dist`) and adjust `DYLD_LIBRARY_PATH` or install to a path that is already on the default search path.

---

## Windows

### Option A: Install from package (.exe / NSIS or .zip)

1. **Build and package:**

   ```cmd
   cmake -B build -S . -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release --target protoCore
   cd build
   cpack
   ```

   CPack generates **NSIS** (.exe) and/or **ZIP** installers.

2. **Install:** Run the NSIS installer or extract the ZIP to a directory (e.g. `C:\Program Files\protoCore`). Ensure the `bin` (or library) directory is on `PATH` if required by your build or runtime.

3. **Verify:** Check that `protoCore.dll` and `protoCore.h` are present and that dependent projects (e.g. protoJS) can find them.

### Option B: Build and install from source

Build as above, then install to a prefix:

```cmd
cmake --install build --config Release --component protoCore --prefix C:\protoCore
```

Add the directory containing `protoCore.dll` to your system or user `PATH` when building or running applications that use protoCore.

---

## Using protoCore in Another Project (e.g. protoJS)

- **Link:** Point your build system to the installed (or built) protoCore library and include directory.
- **Runtime:** Ensure the shared library is on the loader path:
  - **Linux:** `LD_LIBRARY_PATH` or install to a path searched by `ldconfig`.
  - **macOS:** `DYLD_LIBRARY_PATH` or install to a standard location (e.g. `/usr/local/lib`).
  - **Windows:** Add the directory containing `protoCore.dll` to `PATH`.

protoJS looks for `libprotoCore.so` (or `.dylib` / `.dll`) in the protoCore `build/` or `build_check/` directory when built from a sibling repo; for installed protoCore, ensure the library is in a path that the linker and runtime loader use.

---

## Packaging Summary (CPack)

CPack is configured to **generate only packages for the current OS**, so it does not fail when tools for other formats are missing (e.g. on Debian/Ubuntu, RPM is not built unless `rpmbuild` is installed; on Fedora/RHEL, DEB is not built unless `dpkg` is available).

| Platform | Generators (when tools present) | Typical output |
|----------|----------------------------------|----------------|
| Linux (Debian/Ubuntu) | TGZ, DEB | `.tar.gz`, `.deb` |
| Linux (Fedora/RHEL, with rpmbuild) | TGZ, RPM | `.tar.gz`, `.rpm` |
| macOS    | TGZ, DragNDrop | `.tar.gz`, `.dmg` |
| Windows  | ZIP, NSIS | `.zip`, `.exe` |

Packages contain only the **protoCore** component (shared library and public header). To build packages:

```bash
cmake -B build -S .
cmake --build build --target protoCore
cd build && cpack
```

For a specific generator (e.g. DEB only): `cpack -G DEB`.

---

## Troubleshooting

- **Library not found at runtime:** Set `LD_LIBRARY_PATH` (Linux), `DYLD_LIBRARY_PATH` (macOS), or `PATH` (Windows) to the directory containing the shared library, or install to a standard location and run `ldconfig` (Linux).
- **Header not found:** Pass the include directory (e.g. `include/` under your install prefix) to your compiler (`-I` or CMake `include_directories`).
- **CPack fails:** Ensure CMake and the build completed successfully and that you run `cpack` from the same build directory. On Linux, packaging tools (e.g. for DEB/RPM) may need to be installed.

For testing and coverage, see [TESTING.md](TESTING.md) and the [Testing User Guide](Structural%20description/guides/04_testing_user_guide.md).
