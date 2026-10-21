// ExampleSupport.h -- shared by the exception examples.
//
// Loading the geometry module through Unified Module Discovery, calling one
// of its methods, and reading an error's `message`.  Each example checks what
// it shows: a broken expectation prints FAIL and exits non-zero, so the
// examples run as tests.
#ifndef PROTOCORE_EXAMPLE_SUPPORT_H
#define PROTOCORE_EXAMPLE_SUPPORT_H

#include "protoCore.h"
#include "geometry_module/GeometryModule.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace example {

inline const proto::ProtoString* sym(proto::ProtoContext* ctx, const char* name) {
    return proto::ProtoString::createSymbol(ctx, name);
}

// The module's exports: the provider is registered and put first in the
// space's resolution chain.
inline const proto::ProtoObject* loadGeometry(proto::ProtoSpace& space) {
    proto::ProtoContext* ctx = space.rootContext;
    static bool registered = false;
    if (!registered) {
        registerGeometryProvider();
        registered = true;
    }
    const proto::ProtoList* chain = ctx->newList()->appendLast(
        ctx, ctx->fromUTF8String("provider:example_geometry"));
    space.setResolutionChain(chain->asObject(ctx));
    const proto::ProtoObject* wrapper = space.getImportModule(ctx, "example/geometry", "exports");
    return wrapper->getAttribute(ctx, sym(ctx, "exports"));
}

// module.<method>(args...), with the module as the receiver.
inline const proto::ProtoObject* callModule(proto::ProtoContext* ctx, const proto::ProtoObject* module,
                                            const char* method,
                                            std::initializer_list<const proto::ProtoObject*> args) {
    std::vector<const proto::ProtoObject*> a(args);
    return module->call(ctx, nullptr, sym(ctx, method), module, a.data(),
                        static_cast<unsigned>(a.size()));
}

inline std::string messageOf(proto::ProtoContext* ctx, const proto::ProtoObject* error) {
    const proto::ProtoObject* m = error->getAttribute(ctx, sym(ctx, "message"));
    if (!m || m == PROTO_NONE || !m->isString(ctx)) return "<no message>";
    return m->asString(ctx)->toStdString(ctx);
}

inline int g_failures = 0;

inline void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

inline int finish(const char* example) {
    std::printf("%s: %s\n", example, g_failures ? "FAILED" : "PASS");
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

}  // namespace example

#endif
