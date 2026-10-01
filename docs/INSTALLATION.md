# protoCore Installation Guide

This guide covers building protoCore from source, installing the shared library and its public header, and generating packages with CPack. protoCore is required by the runtimes built on it, such as protoJS and protoPython.

---

## Platform Support

- **Linux** with GCC or Clang is the platform these instructions are written for.
- **macOS**: `CMakeLists.txt` configures the TGZ and DragNDrop CPack generators for macOS. This guide does not verify the macOS build.
- **Windows**: native build with MSVC (Visual Studio 2022), verified on Windows 11: the library, the tests (517 of 517 pass), `cmake --install` and the ZIP package. See [Windows (MSVC)](#windows-msvc) below.

No continuous integration is configured in this repository.

---

## Prerequisites

- A C++ compiler with C++20 support (GCC or Clang; MSVC from Visual Studio 2022 on Windows)
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

The library version is whatever `project(... VERSION ...)` in `CMakeLists.txt` says — `2.8.0` as of 2026-10-01 — and its ABI version is `PROTOCORE_ABI_SOVERSION`, currently `3`. Read both out of `CMakeLists.txt` rather than trusting a number repeated here; `<version>` below stands for the first and `3` for the second. On Linux the build directory contains:

| File | Role |
|------|------|
| `build/libprotoCore.so.<version>` | The shared library |
| `build/libprotoCore.so.3` | Link used by the dynamic loader (soname) |
| `build/libprotoCore.so` | Link used at link time (`-lprotoCore`) |

On macOS CMake uses the names `libprotoCore.<version>.dylib`, `libprotoCore.3.dylib` and `libprotoCore.dylib`.

To run the tests after a full build:

```bash
ctest --test-dir build --output-on-failure
```

---

### Windows (MSVC)

protoCore builds natively with Visual Studio 2022 (MSVC 19.44 verified) using
the CMake and Ninja that ship with it. From an "x64 Native Tools Command
Prompt":

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -j8
cmake --install build --prefix %LOCALAPPDATA%\Programs\proto
```

The build produces `protoCore.dll` and its import library `protoCore.lib`.
Executables and the DLL share `build/bin/`, so the tests run in place. On
Windows 11 the whole suite passes (517 of 517).

What differs on Windows, and why Linux and macOS are unaffected:

- The API's 64-bit integers are `proto::proto_long` / `proto::proto_ulong`
  (literals `PROTO_L(x)` / `PROTO_UL(x)`, printf conversion `PROTO_FMT_U`).
  Windows is LLP64 (`long` is 32 bits), so there they are `long long`;
  everywhere else they ARE `long` and `unsigned long`, so the types, the C++
  mangling and the ABI are unchanged (the exported symbols of
  `libprotoCore.so` are identical to those of 2.6.2).
- 128-bit arithmetic in `Integer.cpp` uses MSVC's `std::_Unsigned128`;
  aligned allocation uses `_aligned_malloc`.
- The DLL exports every symbol (`WINDOWS_EXPORT_ALL_SYMBOLS`). Static data a
  caller reads across the DLL boundary is marked `PROTOCORE_DATA`
  (`dllexport` / `dllimport`, empty elsewhere).

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
| Shared library | `lib/libprotoCore.so.<version>` (`2.8.0` as of 2026-10-01), with the links `lib/libprotoCore.so.3` (the soname) and `lib/libprotoCore.so` |
| Public header | `include/protoCore.h` |
| CMake package configuration | `lib/cmake/protoCore/protoCoreConfig.cmake`, `protoCoreConfigVersion.cmake`, `protoCoreTargets.cmake` and one per-configuration targets file |
| pkg-config metadata | `lib/pkgconfig/protoCore.pc` |

The library and header directories come from `GNUInstallDirs`. With the default `/usr/local` prefix they are `lib` and `include`; with other prefixes or distributions the library directory may be `lib64` or a multiarch directory. On Windows the install rules place the DLL in `bin/` and the import library in `lib/`.

## Consuming protoCore from CMake

The install rules export a CMake package configuration, so a consumer asks for
protoCore by name and version rather than searching for files:

```cmake
find_package(protoCore 2.0 REQUIRED CONFIG)
target_link_libraries(my_target PRIVATE protoCore::protoCore)
```

`find_package` uses `CMAKE_PREFIX_PATH` to find a non-default prefix:

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH=$HOME/.local
```

The package provides:

| Name | Meaning |
|------|---------|
| `protoCore::protoCore` | The imported shared library, carrying the include directory and `Threads::Threads` |
| `protoCore_VERSION` | Full version, for example `2.5.0` |
| `protoCore_SOVERSION` | ABI version of the shared library — `PROTOCORE_ABI_SOVERSION`, `3` as of 2026-09-25 |
| `protoCore_INCLUDE_DIR` | Directory holding `protoCore.h` |
| `protoCore_LIB_DIR` | Directory holding the shared library |

Version compatibility is `SameMajorVersion`: a request for `2.0` is satisfied by
any `2.x` and refused for `1.x` and `3.x`. The soname is a separate number —
`PROTOCORE_ABI_SOVERSION`, bumped only when the ABI breaks, and `3` while the
project version is `2.6.2` — so do not infer one from the other. The requested
minor version is still a floor, so
a consumer that needs a feature added in a minor release asks for that release —
protoScala asks for `2.1`, because its actor mailbox needs `ProtoMPSCQueue`,
which protoCore gained in `2.1.0`.

The configuration additionally checks that `libprotoCore.so.3`
(`libprotoCore.3.dylib` on macOS) exists beside it, so a prefix whose CMake
files outlived its library fails with a message rather than a link error.

For consumers that are not CMake projects, `lib/pkgconfig/protoCore.pc` is
installed:

```bash
pkg-config --cflags --libs protoCore
pkg-config --variable=soversion protoCore   # 3
```

## Platform verification status

Last verified 2026-09-27 against protoCore 2.5.0 (`PROTOCORE_ABI_SOVERSION 3`).

| Platform | Packaging | Status |
|----------|-----------|--------|
| Linux / Debian-Ubuntu | TGZ, DEB | **VERIFIED.** Built, then installed with `dpkg -i` as root in a throwaway `ubuntu:24.04` container (glibc 2.39, the same as the build host) and exercised there. |
| Linux / Fedora-RHEL | TGZ, RPM | **VERIFIED.** `cpack -G RPM` executed in a throwaway `fedora:41` container (glibc 2.40, `rpm` 4.20.1); the RPM was installed with `rpm -i` and exercised. This closes the gap left by decision D-I2, under which the RPM generator had been configured but never run on any host. |
| macOS | TGZ, DragNDrop | **UNVERIFIED.** Configured and reviewed only. There is no macOS host here and no cross-toolchain, so the generator has never executed. Review is not verification. |
| Windows | ZIP, NSIS (writes `HKLM\SOFTWARE\protoCore` `Version`, `Soversion`, `InstallDir`) | **PARTLY VERIFIED** (2026-10-01, Windows 11, MSVC 19.44). Built, tested (517/517), installed with `cmake --install` into a user prefix and consumed from there by protoScala; `cpack -G ZIP` produces `protoCore-<version>-win64.zip`. Running cpack also exposed and fixed a quoting defect in the NSIS registry commands. The NSIS installer itself has not been built (no NSIS on that host), so the registry values, and protoJS's WiX condition that reads them, remain unobserved. |

### What the Linux verification actually demonstrated

Each item below was confirmed by also making it fail on purpose, so that a green
result means something:

- `find_package(protoCore 2.0 CONFIG)` accepts the installed 2.5.0 package and
  reports `protoCore_SOVERSION` as `3`. Requesting `3.0` or `1.0` is refused by
  `SameMajorVersion`.
- A consumer binary with no `RPATH` and no `LD_LIBRARY_PATH` resolves
  `libprotoCore.so.3` from the installed prefix; moving that one file away makes
  it fail to start, which is what shows the resolution was real.
- The RPM's automatically generated `Provides: libprotoCore.so.3()(64bit)` is
  what dependent runtimes match on, and a decoy protoCore providing only
  `libprotoCore.so.2` does not satisfy them.

### Known defects in the Linux packages

- **The DEB carries no `postinst` and no `ldconfig` trigger**, so `dpkg -i` does
  not refresh the shared-library cache; `ldconfig -p` does not list
  `libprotoCore.so.3` until `ldconfig` is run by hand. Installed programs still
  start, because the library lands in a directory the dynamic loader searches by
  default, but the cache is misleading and a consumer that relies on it will not
  find the library. The RPM does not have this defect: `rpm` runs `ldconfig`
  itself.
- **`protoCore.pc` is not relocatable.** `prefix=` is expanded from
  `CMAKE_INSTALL_PREFIX` at configure time, so a package configured for one
  prefix and installed under another ships a `.pc` pointing at a directory that
  does not exist on the target. Build packages with
  `-DCMAKE_INSTALL_PREFIX=/usr` so that the `.pc` matches where the DEB and RPM
  actually put the files. The CMake package config does not share this problem;
  it is relocatable through `@PACKAGE_INIT@`.
- **`cmake --install --component protoCore` produces a broken prefix.** The
  exported target set includes `protoCore::protoCoreConformance`, whose static
  library belongs to a different install component, so a component-scoped
  install writes a `protoCoreTargets.cmake` that references a missing file and
  every consumer's `find_package` then fails with a hard error. Install without
  `--component`, or use the DEB/RPM, both of which carry the whole payload.

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

Packages are written to the directory where `cpack` runs.

Build a distributable package with the prefix it will actually be installed
under, because `protoCore.pc` bakes that prefix in at configure time:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
```

Note that on a Debian or Ubuntu host `CMAKE_INSTALL_PREFIX=/usr` also makes
`GNUInstallDirs` select the multiarch library directory, so the payload moves
from `/usr/lib` to `/usr/lib/x86_64-linux-gnu`. That is the correct layout for
those distributions, and every runtime's install `RPATH` is
`$ORIGIN/../${CMAKE_INSTALL_LIBDIR}`, so a runtime built with the same prefix
stays consistent with it. Build protoCore and the runtimes with the same
`CMAKE_INSTALL_PREFIX`.

The package payload is the shared library, `protoCore.h`, the CMake package
configuration, `protoCore.pc`, and also the conformance test library
(`libprotoCoreConformance.a`), its three headers and the
`protocore-conformance-isolate` executable, which embedders use to run the
conformance suite against their own build.

### Package file names

The file names follow `CPACK_PACKAGE_FILE_NAME`, which is `protoCore-<version>-<system>`, where `<version>` is the project version in `CMakeLists.txt` (`2.8.0` as of 2026-10-01):

| Platform | Files |
|----------|-------|
| Linux | `protoCore-<version>-Linux.tar.gz`, `protoCore-<version>-Linux.deb`, `protoCore-<version>-Linux.rpm` |
| macOS | `protoCore-<version>-Darwin.tar.gz`, `protoCore-<version>-Darwin.dmg` |
| Windows | `protoCore-<version>-win64.zip` and an NSIS `.exe` (`win32` on 32-bit builds) |

### Installing and removing the Linux packages

`CPACK_PACKAGE_NAME` is `protoCore`. CMake's DEB and RPM generators use it in lower case, so the installed package is named `protocore`.

**.deb (Debian/Ubuntu):**

```bash
dpkg -c protoCore-<version>-Linux.deb      # list the package contents
sudo dpkg -i protoCore-<version>-Linux.deb
dpkg -L protocore                      # list the installed files
sudo dpkg -r protocore                 # or: sudo apt remove protocore
```

**.rpm (Fedora/RHEL/openSUSE):**

```bash
rpm -qlp protoCore-<version>-Linux.rpm     # list the package contents
sudo rpm -ivh protoCore-<version>-Linux.rpm
rpm -ql protocore                      # list the installed files
sudo rpm -e protocore
```

After installing, run `sudo ldconfig` if dependent programs cannot find `libprotoCore.so.3`.

### Minimal archive: `package_protocore_only`

The custom target `package_protocore_only` builds a tarball without running CPack:

```bash
cmake --build build --target protoCore
cmake --build build --target package_protocore_only
```

It writes `build/protoCore-<version>-Linux.tar.gz` with this layout:

- `protoCore-<version>-Linux/include/protoCore.h`
- `protoCore-<version>-Linux/lib/libprotoCore.so.<version>`

The target copies only the versioned library file (`$<TARGET_FILE:protoCore>`), not the `libprotoCore.so.3` and `libprotoCore.so` links; create them when installing the archive by hand (`ln -s libprotoCore.so.<version> libprotoCore.so.3` and `ln -s libprotoCore.so.3 libprotoCore.so`). The target uses `tar`, names the directory `-Linux` on every platform, and writes to the same file name as the CPack TGZ generator, so running both in the same build directory overwrites one archive with the other.

---

## Using protoCore in Another Project

- **Compile and link:** call `find_package(protoCore 2.0 REQUIRED CONFIG)` and link `protoCore::protoCore` (see "Consuming protoCore from CMake" above). Non-CMake builds use `pkg-config protoCore`.
- **Runtime:** make the shared library visible to the loader:
  - **Linux:** install to a directory known to `ldconfig`, or set `LD_LIBRARY_PATH`. A runtime installed into the same prefix as protoCore needs neither: all five runtimes set `INSTALL_RPATH` to `$ORIGIN/../lib`.
  - **macOS:** install to a standard location (for example `/usr/local/lib`), or set `DYLD_LIBRARY_PATH`. The runtimes set `@executable_path/../lib`.

The five runtimes (protoPython, protoJS, protoST, protoClojure, protoScala) prefer the installed CMake package. When no package is found and no prefix was named, they fall back to a sibling developer build, searching `../protoCore/build_release`, then `../protoCore/build`, then `../protoCore/build_check`; the fallback warns that it performs no version check, and `-DPROTOCORE_REQUIRE_PACKAGE=ON` turns it into an error for packaging builds.

---

## Troubleshooting

- **Library not found at runtime:** set `LD_LIBRARY_PATH` (Linux) or `DYLD_LIBRARY_PATH` (macOS) to the directory containing the shared library, or install to a standard location and run `sudo ldconfig` (Linux).
- **Header not found:** pass the include directory (`include/` under your install prefix) to your compiler (`-I`, or `target_include_directories` in CMake).
- **Configuration fails while fetching GoogleTest:** the first configuration downloads GoogleTest; check network access.
- **CPack fails:** make sure the `protoCore` target is built and that you run `cpack` from the same build directory. DEB and RPM packages need `dpkg` and `rpmbuild` to be installed before configuring.

For testing and coverage, see [TESTING.md](TESTING.md) and the [Testing User Guide](Structural%20description/guides/04_testing_user_guide.md).
