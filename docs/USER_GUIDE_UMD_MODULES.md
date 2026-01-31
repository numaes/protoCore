# User Guide: Generating a Module for Unified Module Discovery

This guide describes how to **generate** (create and register) a module for protoCore’s **Unified Module Discovery** (UMD) system so that modules can be resolved by logical path via `ProtoSpace::getImportModule`.

---

## What you need

- protoCore built and linked.
- A C++ type that implements the **ModuleProvider** interface: `tryLoad(logicalPath, ctx)`, `getGUID()`, `getAlias()`.
- Registration of the provider with **ProviderRegistry** and (optionally) use of the **resolution chain** so your provider is used for certain logical paths.

---

## Steps in brief

1. **Implement a ModuleProvider**  
   Create a class that inherits from `proto::ModuleProvider`, implements `getGUID()` and `getAlias()`, and implements `tryLoad(const std::string& logicalPath, ProtoContext* ctx)` so that it returns a **module object** (a `ProtoObject*` with the module’s “exports”) or `PROTO_NONE` on failure.

2. **Register the provider**  
   At startup, call  
   `ProviderRegistry::instance().registerProvider(std::move(provider));`

3. **Use the resolution chain (optional)**  
   To have UMD use your provider for a given logical path, set the `ProtoSpace` resolution chain to include an entry like `"provider:your_alias"` (or `"provider:GUID"`). If you don’t set the chain, the default (e.g. path entries only) is used.

4. **Load modules**  
   Call `space.getImportModule(logicalPath, "exports")`; the returned wrapper object has an attribute `"exports"` pointing to the module your provider returned (or from cache).

---

## Full guide and specification

- **Detailed user guide (implementation, registration, chain, example):**  
  [Structural description/guides/05_creating_modules.md](Structural%20description/guides/05_creating_modules.md)

- **UMD specification (resolution chain, ProviderRegistry, SharedModuleCache, FileSystemProvider, platform defaults):**  
  [MODULE_DISCOVERY.md](MODULE_DISCOVERY.md)

---

## Quick reference

| Goal | Action |
|------|--------|
| Implement provider | Inherit `ModuleProvider`, implement `tryLoad`, `getGUID`, `getAlias`. |
| Register | `ProviderRegistry::instance().registerProvider(std::move(provider))`. |
| Use in chain | `space.setResolutionChain(ProtoList)` with entry `"provider:alias"` or `"provider:GUID"`. |
| Load module | `space.getImportModule(logicalPath, "exports")`; read wrapper’s `"exports"` attribute. |
