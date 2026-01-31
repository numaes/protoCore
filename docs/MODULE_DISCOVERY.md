# Unified Module Discovery and Provider System

This document describes protoCore's configurable module resolution chain, global provider registry, thread-safe module cache, and the single entry point `ProtoSpace::getImportModule` for resolving and loading modules.

## Overview

The system allows each `ProtoSpace` to have a **resolution chain**: an ordered list of entries that are either path strings (e.g. `"."`, `"/opt/proto/lib"`) or provider specs (e.g. `"provider:odoo_db"`, `"provider:GUID"`). When loading a module by logical path, the runtime walks the chain and asks each entry to resolve the path; the first that returns a module wins (short-circuit). Loaded modules are stored in a **SharedModuleCache** (thread-safe) and registered as GC roots so they are not collected.

## ProviderRegistry (singleton)

- **Access**: `ProviderRegistry::instance()`
- **Register**: `registerProvider(std::unique_ptr<ModuleProvider> provider)` — registry takes ownership.
- **Lookup**: `findByAlias(const std::string& alias)`, `findByGUID(const std::string& guid)` — **alias takes precedence** when resolving a spec.
- **Resolve spec**: `getProviderForSpec(const std::string& spec)` — given `"provider:alias"` or `"provider:GUID"`, returns the corresponding provider (alias tried first, then GUID). Returns `nullptr` if not found or format invalid.

Construction and destruction are thread-safe (e.g. C++11 magic statics).

## ModuleProvider interface

- **Method**: `virtual const ProtoObject* tryLoad(const std::string& logicalPath, ProtoContext* ctx) = 0;`
- **Identity**: `getGUID()` (obligatory), `getAlias()` (optional; used for `provider:alias` lookup).
- **Contract**: Return the module object (any non-`PROTO_NONE` `ProtoObject*`) on success; return `PROTO_NONE` on failure. No exceptions.

## Resolution chain (ProtoSpace)

- **Get**: `const ProtoObject* getResolutionChain() const` — returns the current chain as a `ProtoList` (of `ProtoString` entries). If never set, returns the platform default.
- **Set**: `void setResolutionChain(const ProtoObject* newChain)` — `newChain` must be a `ProtoList` of strings; each element is a path or a `provider:alias` / `provider:GUID` string. If `newChain` is null or invalid, the chain is reset to the platform default.

### Chain entry format

- **Path string**: e.g. `"."`, `"/opt/proto/lib"` — resolved by a `FileSystemProvider` with that base path (resolve `logicalPath` relative to the base; if a file exists, return a module object).
- **Provider spec**: `"provider:alias"` or `"provider:GUID"` — the corresponding provider is looked up and `tryLoad(logicalPath, ctx)` is called.

## ProtoSpace::getImportModule

Module access and loading are on **ProtoSpace** (not on a separate object), so that any language or host can use the same space-scoped resolution.

```cpp
const ProtoObject* wrapper = space.getImportModule(logicalPath, attrName2create);
```

- **Cache**: If `logicalPath` is already in the SharedModuleCache, a wrapper object is built with attribute `attrName2create` pointing to the cached module and returned (no search).
- **Search**: Otherwise, iterate `space.getResolutionChain()` in order. For each entry, resolve (path via FileSystemProvider, or provider spec via registry) and call the provider’s `tryLoad(logicalPath, space.rootContext)`. **Short-circuit**: the first result that is not `PROTO_NONE` is used.
- **Success**: Build an immutable wrapper `ProtoObject` with attribute `attrName2create` pointing to the found module; insert the module into SharedModuleCache; add the module to `space.moduleRoots` (GC roots); return the wrapper.
- **Failure**: Return `PROTO_NONE` (no exceptions).

Thread-safe: cache uses `std::shared_mutex` (multiple readers, exclusive writer); module roots are updated under `ProtoSpace::globalMutex`.

## SharedModuleCache

- **Key**: `std::string(logicalPath)`
- **Value**: `const ProtoObject*` (the loaded module)
- **Concurrency**: `std::shared_mutex` — shared lock for get, unique lock for insert.
- **Lifecycle**: Cached pointers are kept alive via `space.moduleRoots` (scanned by GC). Cache is global (same key across spaces returns the same cached module).

## FileSystemProvider

- **Role**: Default handling for path entries. Given a base path (e.g. from chain entry `"."`), resolves `logicalPath` relative to that base; if the result is an existing file, returns a minimal module object (e.g. with a `"path"` attribute).
- **Scope**: protoCore’s FileSystemProvider is minimal (path resolution and placeholder module); full native/script loading is the responsibility of host runtimes (e.g. protoJS, protoPython).

## Platform-default resolution chain

If the chain is not set (or is set to null), a platform-dependent default is used:

| Platform | Default entries |
|----------|------------------|
| Linux   | `"."`, `"/usr/lib/proto"`, `"/usr/local/lib/proto"` |
| Windows | `"."`, `"C:\\Program Files\\proto\\lib"` |
| macOS   | `"."`, `"/usr/local/lib/proto"` |

## ProtoString::toUTF8String

`void ProtoString::toUTF8String(ProtoContext* context, std::string& out) const` — appends the UTF-8 representation of the string to `out`. Used by the resolver to interpret chain entries (e.g. `"provider:alias"`) and path strings.

## Guidelines

- **Isolation**: Use a project-local resolution chain (e.g. `["."]`) so that only local modules are loaded.
- **Polylingual**: protoJS and protoPython can both call `ProtoSpace::getImportModule` (on their space); the result is a `ProtoObject` that both can map to their native module representation.
- **Extensibility**: Register custom providers (e.g. `OdooProvider` that loads from a DB); put them at the front of the chain to override file-based resolution.

## Example

```cpp
// Register a custom provider
auto provider = std::make_unique<MyProvider>("my-guid", "my_alias");
ProviderRegistry::instance().registerProvider(std::move(provider));

// Set chain: first try custom provider, then current directory
const ProtoList* chain = ctx->newList();
chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:my_alias"));
chain = chain->appendLast(ctx, ctx->fromUTF8String("."));
space.setResolutionChain(chain->asObject(ctx));

// Load module
const ProtoObject* result = space.getImportModule("my_module", "exports");
if (result != PROTO_NONE) {
    const ProtoObject* exports = result->getAttribute(ctx, ProtoString::fromUTF8String(ctx, "exports"));
    // use exports...
}
```

## See also

- **[User Guide: Generating a Module for UMD](USER_GUIDE_UMD_MODULES.md)** — Short user guide (steps, quick reference, links to full guide and spec).
- **[User Guide: Creating Modules](Structural%20description/guides/05_creating_modules.md)** — Step-by-step guide to implementing a custom `ModuleProvider`, registering it, and loading modules with examples.
