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
const ProtoObject* getImportModule(ProtoContext* context, const char* logicalPath, const char* attrName2create);

const ProtoObject* wrapper = space.getImportModule(context, logicalPath, attrName2create);
```

`context` is a `ProtoContext*` of the calling thread (for example `space.rootContext` on the main thread). It is used for every allocation and is passed to the providers.

- **Invalid arguments**: If `context`, `logicalPath` or `attrName2create` is null, returns `PROTO_NONE`.
- **Search**: Iterate `space.getResolutionChain()` in order. For each entry, resolve its provider first (a path via `FileSystemProvider`, a `provider:` spec via the registry), build the module's `ModuleIdentity` from **that provider's GUID**, and probe the SharedModuleCache with it. On a hit, use the cached module; on a miss, call the provider's `tryLoad(logicalPath, context)`. **Short-circuit**: the first result that is not `PROTO_NONE` is used.
- **Success**: insert the module into the SharedModuleCache under its identity, root it with `space.addModuleRoot(module)`, and build an immutable wrapper `ProtoObject` (parented to `space.objectPrototype`) with attribute `attrName2create` pointing to the module.
- **Failure**: Return `PROTO_NONE` (no exceptions).

Thread-safe: the cache uses `std::shared_mutex` (multiple readers, exclusive writer); the module root table appends under a per-shard mutex.

**Since protoCore 2.2.0 the cache probe happens PER RESOLUTION-CHAIN ENTRY, not once before the loop.** It has to: a module's identity includes its provider, and the provider is not known until an entry is selected. That changes resolution in one observable way, and it is a fix — **a module already loaded from a LATER chain entry no longer shadows an EARLIER entry that can serve the same path**, because the earlier entry is probed with its own provider's key. Chain order is the user's stated precedence. Pinned by `ModuleDiscoveryTest.AnEarlierChainEntryIsNotShadowedByAnEarlierLoad`.

**The wrapper is now built once for both the hit and the miss**, and it is parented to `space.objectPrototype` in both. Before 2.2.0 only the cache-hit branch did that, so the same call returned a wrapper with a different prototype chain depending on whether the module happened to be cached already.

## Module identity: provider + path + version

Ruled by the maintainer on 2026-09-24. **`ModuleIdentity`** (`headers/protoCore.h`) is the key of the process-global module list:

```
<providerGUID> "\x1F" <logicalPath> "\x1F" <version>
```

- `\x1F` (ASCII unit separator) is the separator because it cannot occur in a provider GUID, in a POSIX or Windows path, or in any version syntax — unlike `/`, which is in every path, and `:`, which is in the `provider:` spec and in Windows drive letters.
- The provider component is the provider's **GUID** (`ModuleProvider::getGUID()`), never its alias: an alias is a user-facing nickname that can be re-pointed at a different provider, a GUID is stable. For a filesystem chain entry it is that `FileSystemProvider`'s GUID, which is derived from its base path — so `./counter_lib` and `/usr/share/counter_lib` are two modules, which is correct.
- **A module that declares no version has the EMPTY version**, and that is a permanent, first-class identity value meaning *"declares no version"*. It is **not** a wildcard and **not** a synonym for any declared version. There is no module manifest yet; fixing the key's *shape* now is what guarantees that introducing one later cannot re-alias any module that exists today, because an unversioned key is byte-identical before and after. **A future resolver MUST reject an empty declared version**, because `""` is reserved. `"0.0.0"`, `"unversioned"` and `"latest"` were rejected as the reserved value, because each is a string a future manifest could legitimately declare.
- Modules are process-level and perennial — they never unload — so two coexisting versions of one module are two modules for the life of the process, and identity must be able to tell them apart. Diamond dependencies will produce exactly that once modules have dependencies.

**What this fixed.** The cache used to be keyed by the path alone with the provider prefix stripped, so `provider:st/counter_lib` and a local `counter_lib` collapsed into **one** module with the first load winning for both — a wrong answer with no error.

## The module list is a process-global GC root

**`SharedModuleCache` holds the IDENTITY; `ModuleRootTable` holds the REACHABILITY.** Two structures, two jobs.

- **`ModuleRootTable`** (`core/ModuleRoots.cpp`, declared in `headers/proto_internal.h`) is the process-global list of loaded modules, and it **is a GC root**. 8 shards, append-only chunks that never move, a per-shard published count stored with release ordering after the entry is written.
- **Historically, `SharedModuleCache` alone was NEVER a root.** It was already process-global and held raw module pointers no collector ever read. Retention came from `getImportModuleImpl` pushing the module into the **calling** space's `moduleRoots` — once per importing space, not globally — and a prefixed cross-runtime import that called a provider directly reached neither mechanism at all.
- **Why a root and not merely unfreed memory.** A perennial cell is never swept, but it is also never **scanned**: the references it holds do not keep their targets alive. A symbol gets away with perennial allocation alone because its nodes are all perennial too. A module object's contents are **ordinary collectable objects** in a space's heap, so an unfreed list of modules would keep the list alive and let the collector free everything it points at. The mark must ENTER through the table.
- **Per-owner tracing.** Each entry records the `ProtoSpace` whose heap holds the module, and the Phase-4 walk visits only the collecting space's own entries. The table is global — a module is found once for the process — while the tracing stays where the cells are, so a collector never traverses another space's heap on account of this table.
- **Pause cost.** GC Phase 2 calls `captureForGC()` under stop-the-world: 8 counter reads, no entry dereferenced. Phase 4 pushes the captured entries after the world resumes. See `docs/GarbageCollector.md`.
- **Append-only by design**, because a loaded module never unloads. The table is therefore **not** a general-purpose embedder root set: anything that must be unpinned belongs in a `ProtoRootSet`. `~ProtoSpace` calls `purgeSpace` to retire its own entries, because an entry must not outlive the space it names — the allocator can hand a later `ProtoSpace` the same address.

## How an embedder participates

```cpp
void               ProtoSpace::addModuleRoot(const ProtoObject* module);
const ProtoObject* ProtoSpace::registerModule(const ModuleIdentity& id, const ProtoObject* module);
static const ProtoObject* ProtoSpace::findModule(const ModuleIdentity& id);
static unsigned long      ProtoSpace::moduleRootCount();
```

`registerModule` is for an embedder that calls a `ModuleProvider` **directly** instead of going through `getImportModule` — protoScala's `Session::loadForeign` is the case that motivated it. Before 2.2.0 such a load reached neither the cache nor any roots, and its only anchor was inside the **providing** runtime, so destroying that runtime dropped the module while the importer still held its values. `registerModule` is publish-or-adopt: if the identity is already served it roots and returns the existing module, so two importers of one identity share one module.

## SharedModuleCache

- **Key**: `ModuleIdentity::asKey()` — provider GUID + logical path + version. **Not** the bare path.
- **Value**: `const ProtoObject*` (the loaded module)
- **Concurrency**: `std::shared_mutex` — shared lock for get, unique lock for insert.
- **Lifecycle**: the cache is process-global and is **not** a GC root. Reachability comes from `ModuleRootTable`, which is. A module registered by a space that is later destroyed stays in the cache; under the perennial model a loaded module never unloads, so a space dying while holding modules is outside the model, and `findModule` on such an identity is not safe to dereference. That is a pre-existing property of a process-global cache, not something 2.2.0 introduced.

## FileSystemProvider

- **Role**: Default handling for path entries. Given a base path (e.g. from chain entry `"."`), resolves `logicalPath` relative to that base; if the result is an existing file, returns a minimal module object with a `"path"` attribute.
- **Scope**: protoCore's FileSystemProvider is minimal (path resolution and placeholder module); full native/script loading is the responsibility of host runtimes (e.g. protoJS, protoPython).

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
- **Extensibility**: Register custom providers (e.g. an `OdooProvider` that loads from a database); put them at the front of the chain to override file-based resolution.

## Example

```cpp
// Register a custom provider (MyProvider implements proto::ModuleProvider)
auto provider = std::make_unique<MyProvider>("my-guid", "my_alias");
ProviderRegistry::instance().registerProvider(std::move(provider));

// Use the root context of the space on the main thread
ProtoContext* ctx = space.rootContext;

// Set chain: first try custom provider, then current directory
const ProtoList* chain = ctx->newList();
chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:my_alias"));
chain = chain->appendLast(ctx, ctx->fromUTF8String("."));
space.setResolutionChain(chain->asObject(ctx));

// Load module
const ProtoObject* result = space.getImportModule(ctx, "my_module", "exports");
if (result != PROTO_NONE) {
    const ProtoObject* exports = result->getAttribute(ctx, ProtoString::fromUTF8(ctx, "exports"));
    // use exports...
}
```

## See also

- **[User Guide: Generating a Module for UMD](USER_GUIDE_UMD_MODULES.md)** — Short user guide (steps, quick reference, links to full guide and spec).
- **[User Guide: Creating Modules](Structural%20description/guides/05_creating_modules.md)** — Step-by-step guide to implementing a custom `ModuleProvider`, registering it, and loading modules with examples.
