# User Guide: Creating Modules in protoCore

This guide explains how to create and register custom modules in protoCore using the **Unified Module Discovery** system. You will implement a **ModuleProvider**, register it, and load modules via `ProtoSpace::getImportModule`. For the full specification of the resolution chain, cache, and APIs, see [MODULE_DISCOVERY.md](../../MODULE_DISCOVERY.md).

---

## Prerequisites

- protoCore built and linked (see [Quick Start](01_quick_start.md)).
- Include `protoCore.h` and use a valid `ProtoContext*` and `ProtoSpace*` (e.g. from your host application or test).

---

## 1. Concepts

- **ModuleProvider**: An abstract interface that loads a module for a given *logical path*. You implement `tryLoad(logicalPath, ctx)` and return a **module object** (any `ProtoObject*` except `PROTO_NONE`) or `PROTO_NONE` on failure.
- **ProviderRegistry**: A global singleton that stores registered providers. Each provider has a **GUID** (required) and an optional **alias** (e.g. `"my_provider"`). The resolution chain can reference a provider with `"provider:alias"` or `"provider:GUID"`.
- **Resolution chain**: A list of entries (paths or `provider:...` specs) attached to each `ProtoSpace`. When loading a module, the runtime walks the chain and asks each entry to resolve the path; the first non-`PROTO_NONE` result wins.
- **space.getImportModule(logicalPath, attrName2create)**: The single entry point to load a module (on ProtoSpace). It checks the shared cache first; on miss, it walks the resolution chain, calls the appropriate provider’s `tryLoad`, caches the result, registers it as a GC root, and returns a **wrapper** object whose attribute `attrName2create` (e.g. `"exports"`) points to the loaded module.

The object you return from `tryLoad` is the **module content** (e.g. the “exports” of the module). The runtime stores it in the cache and exposes it to the host as the value of the wrapper’s `attrName2create` attribute.

---

## 2. Implementing a ModuleProvider

Your provider must:

1. Inherit from `proto::ModuleProvider`.
2. Implement `getGUID()` and `getAlias()` (alias can be an empty string if you only use GUID).
3. Implement `tryLoad(const std::string& logicalPath, ProtoContext* ctx)`:
   - Return a **module object** (e.g. a `ProtoObject` with attributes representing the module’s exports), or
   - Return `PROTO_NONE` if the path is not handled by this provider.

Do not throw; keep allocations minimal so that failures can be expressed by returning `PROTO_NONE`.

**Example: Greeter provider**

A minimal provider that only resolves the logical path `"greeter"` and returns a simple module object with a string attribute `"message"`:

```cpp
#include "headers/protoCore.h"
#include <string>

namespace myapp {

class GreeterProvider : public proto::ModuleProvider {
public:
    GreeterProvider() : guid_("myapp.greeter.1"), alias_("greeter") {}

    const proto::ProtoObject* tryLoad(const std::string& logicalPath, proto::ProtoContext* ctx) override {
        if (logicalPath != "greeter")
            return proto::PROTO_NONE;

        const proto::ProtoObject* module = ctx->newObject(false);
        if (!module) return proto::PROTO_NONE;

        const proto::ProtoString* key = proto::ProtoString::fromUTF8String(ctx, "message");
        const proto::ProtoObject* value = ctx->fromUTF8String("Hello from the Greeter module!");
        if (!key || !value) return proto::PROTO_NONE;

        module = module->setAttribute(ctx, key, value);
        return module;
    }

    const std::string& getGUID() const override { return guid_; }
    const std::string& getAlias() const override { return alias_; }

private:
    std::string guid_;
    std::string alias_;
};

} // namespace myapp
```

---

## 3. Registering the Provider

Register your provider with the global registry (e.g. at startup, before any `space.getImportModule` call):

```cpp
#include "headers/protoCore.h"

int main() {
    proto::ProtoContext context;
    proto::ProtoSpace space;

    auto provider = std::make_unique<myapp::GreeterProvider>();
    proto::ProviderRegistry::instance().registerProvider(std::move(provider));

    // Optional: set resolution chain so that "provider:greeter" is used (see below)
    // ...

    const proto::ProtoObject* wrapper = space.getImportModule("greeter", "exports");
    if (wrapper != proto::PROTO_NONE) {
        const proto::ProtoString* exportsName = proto::ProtoString::fromUTF8String(space.rootContext, "exports");
        const proto::ProtoObject* exports = wrapper->getAttribute(space.rootContext, exportsName);
        // exports is the module object returned by GreeterProvider::tryLoad
        // (e.g. an object with attribute "message")
    }
    return 0;
}
```

---

## 4. Using the Resolution Chain

By default, the resolution chain is **platform-dependent** (e.g. `"."`, `"/usr/lib/proto"`, …). To use your provider, you can:

- **Option A**: Rely on the default chain and only use the **FileSystemProvider** (path entries). Your custom provider is used only if you add a **provider spec** to the chain.
- **Option B**: Set a custom chain that includes `"provider:greeter"` (or `"provider:GUID"`) so that `space.getImportModule` calls your provider for the given logical path.

**Example: custom chain with provider first**

```cpp
proto::ProtoContext* ctx = space.rootContext;
const proto::ProtoList* chain = ctx->newList();
chain = chain->appendLast(ctx, ctx->fromUTF8String("provider:greeter"));
chain = chain->appendLast(ctx, ctx->fromUTF8String("."));
space.setResolutionChain(chain->asObject(ctx));

// Now space.getImportModule("greeter", "exports") will call GreeterProvider::tryLoad("greeter", ctx).
```

Alias takes precedence: if both alias and GUID match a spec, the provider registered with that alias is used.

---

## 5. Loading Modules

Use the single entry point:

```cpp
const proto::ProtoObject* wrapper = space.getImportModule("greeter", "exports");
```

- **Cache**: If `"greeter"` was already loaded, `wrapper` is a new wrapper object whose `"exports"` attribute points to the cached module; no provider is called again.
- **First load**: The runtime walks the resolution chain, calls the matching provider’s `tryLoad("greeter", ctx)`, inserts the returned module into the shared cache, adds it to the space's moduleRoots (GC roots), and returns a wrapper with `"exports"` set to that module.
- **Failure**: Returns `PROTO_NONE` (no exceptions).

Host runtimes (e.g. protoJS) map the wrapper’s `"exports"` attribute to their native module representation (e.g. a JS object).

---

## 6. File-Based Modules (Default Chain)

If you do not register custom providers, the default chain uses **path entries** only (e.g. `"."`, `"/usr/lib/proto"`). Those are handled by the built-in **FileSystemProvider**: for each path entry, it resolves `logicalPath` relative to that path; if the result is an existing file, it returns a minimal module object (e.g. with a `"path"` attribute). Full loading of native/script files is the responsibility of host runtimes (protoJS, protoPython). To add a custom directory to the chain:

```cpp
const proto::ProtoList* chain = ctx->newList();
chain = chain->appendLast(ctx, ctx->fromUTF8String("/opt/myapp/modules"));
chain = chain->appendLast(ctx, ctx->fromUTF8String("."));
space.setResolutionChain(chain->asObject(ctx));
```

---

## 7. Summary

| Step | Action |
|------|--------|
| 1 | Implement a class that inherits `ModuleProvider` and implements `tryLoad`, `getGUID`, `getAlias`. |
| 2 | Register it with `ProviderRegistry::instance().registerProvider(std::move(provider))`. |
| 3 | (Optional) Set `space.setResolutionChain(...)` with an entry `"provider:alias"` or `"provider:GUID"` so your provider is used for certain logical paths. |
| 4 | Load modules with `space.getImportModule(logicalPath, "exports")` and read the wrapper’s `"exports"` attribute to get the module object. |

---

## References

- [MODULE_DISCOVERY.md](../../MODULE_DISCOVERY.md) — Full specification of the Unified Module Discovery system (resolution chain, ProviderRegistry, SharedModuleCache, ProtoSpace::getImportModule, FileSystemProvider, platform defaults).
- [Building on protoCore](02_building_on_proto.md) — Direct C++ integration and context/space usage.
