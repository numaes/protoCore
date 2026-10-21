// 04_foreign_cpp_exceptions.cpp -- tutorial 04 (docs/tutorials/exceptions/).
//
// A C++ library that throws std::runtime_error is not raising a language
// value.  A runtime translates it once, where it calls native code: a
// std::exception becomes the language's generic error with what() as the
// message, a ProtoException passes through untouched, and anything else is
// reported at the top level instead of ending in std::terminate.
#include "ExampleSupport.h"

#include <stdexcept>

using namespace proto;
using namespace example;

namespace {

const ProtoObject* g_RuntimeError = nullptr;   // the language's generic error

// The native-call boundary of a runtime.  A ProtoException is already a
// language value and must not be caught here.
const ProtoObject* callNative(ProtoContext* ctx, const ProtoObject* module, const char* method,
                              std::initializer_list<const ProtoObject*> args) {
    try {
        return callModule(ctx, module, method, args);
    } catch (const ProtoException&) {
        throw;   // explicit for the reader: a language value passes unchanged
    } catch (const std::exception& e) {
        const ProtoObject* error = g_RuntimeError->newChild(ctx)->setAttribute(
            ctx, sym(ctx, "message"), ctx->fromUTF8String(e.what()));
        ctx->raise(error);
    }
}

}  // namespace

int main() {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoObject* geometry = loadGeometry(space);
    const ProtoObject* GeometryError = geometry->getAttribute(ctx, sym(ctx, "GeometryError"));
    g_RuntimeError = ctx->newObject(false);
    ProtoRootSet* roots = space.createRootSet("example-language");
    roots->add(g_RuntimeError);

    // 1. Without the boundary, the library's exception is invisible to a
    //    language handler: it is not a ProtoException.
    bool sawProto = false, sawStd = false;
    try {
        try {
            (void) callModule(ctx, geometry, "legacyArea", {ctx->fromInteger(-1)});
        } catch (const ProtoException&) {
            sawProto = true;
        }
    } catch (const std::exception&) {
        sawStd = true;
    }
    check(!sawProto && sawStd, "a std::runtime_error goes past every language handler");

    // 2. With the boundary, it is the language's RuntimeError.
    try {
        (void) callNative(ctx, geometry, "legacyArea", {ctx->fromInteger(-1)});
    } catch (const ProtoException& e) {
        check(e.isInstanceOf(ctx, g_RuntimeError), "translated into the language's RuntimeError");
        check(messageOf(ctx, e.value()) == "legacyArea: negative radius", "... with what() as the message");
    }

    // 3. A language value crosses the boundary untouched.
    try {
        (void) callNative(ctx, geometry, "circleArea", {ctx->fromInteger(-1)});
    } catch (const ProtoException& e) {
        check(e.isInstanceOf(ctx, GeometryError), "a module's own error is not translated");
        check(!e.isInstanceOf(ctx, g_RuntimeError), "... and is not a RuntimeError");
    }

    // 4. The top level: report, never std::terminate.
    int exitCode = 0;
    try {
        throw 42;   // what a careless library may throw
    } catch (const ProtoException&) {
        exitCode = 1;   // an uncaught language error: print it the language's way
    } catch (const std::exception&) {
        exitCode = 2;
    } catch (...) {
        exitCode = 3;   // "uncaught foreign exception"
    }
    check(exitCode == 3, "the top level reports a foreign exception instead of terminating");

    return finish("04_foreign_cpp_exceptions");
}
