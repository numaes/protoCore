// 01_catch_by_prototype.cpp -- tutorial 01 (docs/tutorials/exceptions/).
//
// A C++ embedder calls a module method that raises the module's own error
// type, and selects the error by prototype: the handler for the base type
// catches every subtype, and a handler for an unrelated type does not run.
#include "ExampleSupport.h"

using namespace proto;
using namespace example;

int main() {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoObject* geometry = loadGeometry(space);

    // The module's error types are ordinary exported values.
    const ProtoObject* GeometryError = geometry->getAttribute(ctx, sym(ctx, "GeometryError"));
    const ProtoObject* NegativeRadius = geometry->getAttribute(ctx, sym(ctx, "NegativeRadius"));
    const ProtoObject* DegenerateTriangle = geometry->getAttribute(ctx, sym(ctx, "DegenerateTriangle"));

    // 1. No error: the call answers a value.
    const ProtoObject* area = callModule(ctx, geometry, "circleArea", {ctx->fromInteger(2)});
    check(area->asDouble(ctx) > 12.56 && area->asDouble(ctx) < 12.57, "circleArea(2) is 4*pi");

    // 2. The module raises NegativeRadius.  One C++ catch, of the one type;
    //    the selection is the value's prototype chain.
    try {
        (void) callModule(ctx, geometry, "circleArea", {ctx->fromInteger(-1)});
        check(false, "circleArea(-1) raises");
    } catch (const ProtoException& e) {
        check(e.isInstanceOf(ctx, NegativeRadius), "the value is a NegativeRadius");
        check(e.isInstanceOf(ctx, GeometryError), "... and therefore a GeometryError");
        check(!e.isInstanceOf(ctx, DegenerateTriangle), "... and not a DegenerateTriangle");
        std::printf("      message: %s\n", messageOf(ctx, e.value()).c_str());
    }

    // 3. A handler written for the base type catches a subtype it never named.
    auto areaOrZero = [&](double a, double b, double c) -> double {
        try {
            return callModule(ctx, geometry, "triangleArea",
                              {ctx->fromDouble(a), ctx->fromDouble(b), ctx->fromDouble(c)})
                ->asDouble(ctx);
        } catch (const ProtoException& e) {
            if (!e.isInstanceOf(ctx, GeometryError)) throw;   // not ours: let it go
            return 0.0;
        }
    };
    check(areaOrZero(3, 4, 5) == 6.0, "triangleArea(3, 4, 5) is 6");
    check(areaOrZero(1, 2, 10) == 0.0, "a degenerate triangle is caught as a GeometryError");

    // 4. A handler for an unrelated type rethrows; the same value arrives
    //    at the next handler out.
    const ProtoObject* NetworkError = ctx->newObject(false);   // another module's type
    const ProtoObject* inner = nullptr;
    try {
        try {
            (void) callModule(ctx, geometry, "circleArea", {ctx->fromInteger(-5)});
        } catch (const ProtoException& e) {
            inner = e.value();
            if (!e.isInstanceOf(ctx, NetworkError)) throw;
            check(false, "a NetworkError handler must not take a geometry error");
        }
    } catch (const ProtoException& e) {
        check(e.value() == inner, "the rethrown exception carries the same value");
    }

    return finish("01_catch_by_prototype");
}
