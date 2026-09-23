# protoCore Installation Guide

This guide covers building protoCore from source, installing the shared library and its public header, and generating packages with CPack. protoCore is required by the runtimes built on it, such as protoJS and protoPython.

---

## Platform Support

- **Linux** with GCC or Clang is the platform these instructions are written for.
- **macOS**: `CMakeLists.txt` configures the TGZ and DragNDrop CPack generators for macOS. This guide does not verify the macOS build.
- **Windows**: `CMakeLists.txt` configures the ZIP and NSIS CPack generators for Windows, but the cell allocator calls `posix_memalign` (`core/ProtoSpace.cpp`, `core/ProtoContext.cpp`), which the Microsoft C runtime does not provide, and `CMakeLists.txt` adds the GCC/Clang option `-fno-delete-null-pointer-checks` for every compiler. A native MSVC build is not expected to work without source changes.

No continuous integration is configured in this repository.

---

## Prerequisites

- A C++ compiler with C++20 support (GCC or Clang)
- **CMake** 3.16 or later (`cmake_minimum_required` in `CMakeLists.txt`)
- A threads library (`find_package(Threads REQUIRED)`)
- Network access during the first configuration: `test/CMakeLists.txt` downloads GoogleTest 1.14.0 with `FetchContent`

---

## Building from Source

From the protoCore project root:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds the shared library, the test executable `build/test/proto_tests` and the benchmark executables. To build only the library:

```bash
cmake --build build --target protoCore
```

The library version is `2.0.0` (`project(... VERSION 2.0.0)`) and its ABI version is `2` (`SOVERSION 2`). On Linux the build directory contains:

| File | Role |
|------|------|
| `build/libprotoCore.so.2.0.0` | The shared library |
| `build/libprotoCore.so.2` | Link used by the dynamic loader (soname) |
| `build/libprotoCore.so` | Link used at link time (`-lprotoCore`) |

On macOS CMake uses the names `libprotoCore.2.0.0.dylib`, `libprotoCore.2.dylib` and `libprotoCore.dylib`.

To run the tests after a full build:

```bash
ctest --test-dir build --output-on-failure
```

---

## Installing the Built Library

The install rules belong to the `protoCore` component and are defined only when protoCore is the top-level CMake project (a project that includes protoCore with `add_subdirectory` handles its own packaging).

**Staging install** (for packaging or local use):

```bash
cmake --install build --component protoCore --prefix ./dist
```

**System install** (default prefix `/usr/local`; requires appropriate privileges):

```bash
sudo cmake --install build --component protoCore
sudo ldconfig
```

**Installed files** (Linux, paths relative to the prefix):

| File | Path |
|------|------|
| Shared library | `lib/libprotoCore.so.2.0.0`, with the links `lib/libprotoCore.so.2` and `lib/libprotoCore.so` |
| Public header | `include/protoCore.h` |

The library and header directories come from `GNUInstallDirs`. With the default `/usr/local` prefix they are `lib` and `include`; with other prefixes or distributions the library directory may be `lib64` or a multiarch directory. On Windows the install rules place the DLL in `bin/` and the import library in `lib/`.

The install rules export no CMake package configuration file, so consumers locate protoCore with `find_library` and `find_path` (or `-I<prefix>/include -L<prefix>/lib -lprotoCore`).

---

## Packages (CPack)

### Configured generators

`CMakeLists.txt` selects the CPack generators at configure time, only for the current platform, so `cpack` does not fail when tools for other formats are missing:

| Platform | Generators |
|----------|------------|
| Linux | TGZ; DEB when `dpkg` is found; RPM when `rpmbuild` is found |
| macOS | TGZ, DragNDrop |
| Windows | ZIP, NSIS |

On Linux the configure output reports the extra generators, for example `CPack: DEB generator enabled (dpkg found)`.

### Building packages

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --target protoCore
cd build
cpack            # every configured generator
cpack -G DEB     # a single generator
```

Packages are written to the directory where `cpack` runs. The only install rules in a top-level build are those for the library and `protoCore.h` (`test/CMakeLists.txt` forces `INSTALL_GTEST` off, and the test and benchmark executables have no install rules), so the packages contain only the library files and the public header.

### Package file names (version 2.0.0)

The file names follow `CPACK_PACKAGE_FILE_NAME`, which is `protoCore-2.0.0-<system>`:

| Platform | Files |
|----------|-------|
| Linux | `protoCore-2.0.0-Linux.tar.gz`, `protoCore-2.0.0-Linux.deb`, `protoCore-2.0.0-Linux.rpm` |
| macOS | `protoCore-2.0.0-Darwin.tar.gz`, `protoCore-2.0.0-Darwin.dmg` |
| Windows | `protoCore-2.0.0-win64.zip` and an NSIS `.exe` (`win32` on 32-bit builds) |

### Installing and removing the Linux packages

`CPACK_PACKAGE_NAME` is `protoCore`. CMake's DEB and RPM generators use it in lower case, so the installed package is named `protocore`.

**.deb (Debian/Ubuntu):**

```bash
dpkg -c protoCore-2.0.0-Linux.deb      # list the package contents
sudo dpkg -i protoCore-2.0.0-Linux.deb
dpkg -L protocore                      # list the installed files
sudo dpkg -r protocore                 # or: sudo apt remove protocore
```

**.rpm (Fedora/RHEL/openSUSE):**

```bash
rpm -qlp protoCore-2.0.0-Linux.rpm     # list the package contents
sudo rpm -ivh protoCore-2.0.0-Linux.rpm
rpm -ql protocore                      # list the installed files
sudo rpm -e protocore
```

After installing, run `sudo ldconfig` if dependent programs cannot find `libprotoCore.so.2`.

### Minimal archive: `package_protocore_only`

The custom target `package_protocore_only` builds a tarball without running CPack:

```bash
cmake --build build --target protoCore
cmake --build build --target package_protocore_only
```

It writes `build/protoCore-2.0.0-Linux.tar.gz` with this layout:

- `protoCore-2.0.0-Linux/include/protoCore.h`
- `protoCore-2.0.0-Linux/lib/libprotoCore.so.2.0.0`

The target copies only the versioned library file (`$<TARGET_FILE:protoCore>`), not the `libprotoCore.so.2` and `libprotoCore.so` links; create them when installing the archive by hand (`ln -s libprotoCore.so.2.0.0 libprotoCore.so.2` and `ln -s libprotoCore.so.2 libprotoCore.so`). The target uses `tar`, names the directory `-Linux` on every platform, and writes to the same file name as the CPack TGZ generator, so running both in the same build directory overwrites one archive with the other.

---

## Using protoCore in Another Project

- **Compile and link:** add the installed (or build) include directory and library directory to your build, and link with `protoCore`.
- **Runtime:** make the shared library visible to the loader:
  - **Linux:** install to a directory known to `ldconfig`, or set `LD_LIBRARY_PATH`.
  - **macOS:** install to a standard location (for example `/usr/local/lib`), or set `DYLD_LIBRARY_PATH`.

protoJS's `CMakeLists.txt`, when no installed protoCore is configured, looks for the library in the sibling directories `../protoCore/build` and `../protoCore/build_check`.

---

## Troubleshooting

- **Library not found at runtime:** set `LD_LIBRARY_PATH` (Linux) or `DYLD_LIBRARY_PATH` (macOS) to the directory containing the shared library, or install to a standard location and run `sudo ldconfig` (Linux).
- **Header not found:** pass the include directory (`include/` under your install prefix) to your compiler (`-I`, or `target_include_directories` in CMake).
- **Configuration fails while fetching GoogleTest:** the first configuration downloads GoogleTest; check network access.
- **CPack fails:** make sure the `protoCore` target is built and that you run `cpack` from the same build directory. DEB and RPM packages need `dpkg` and `rpmbuild` to be installed before configuring.

For testing and coverage, see [TESTING.md](TESTING.md) and the [Testing User Guide](Structural%20description/guides/04_testing_user_guide.md).
