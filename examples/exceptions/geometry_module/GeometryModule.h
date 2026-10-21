// GeometryModule.h -- the example module of docs/tutorials/exceptions/.
//
// A UMD module, built as its own shared library, that defines an error type
// hierarchy and methods that raise it:
//
//     GeometryError                  the module's base error type
//      +-- NegativeRadius            circleArea(r) with r < 0
//      +-- DegenerateTriangle        triangleArea(a, b, c) when the sides
//                                    do not make a triangle
//
// Exports, all attributes of the module object:
//   GeometryError, NegativeRadius, DegenerateTriangle   the error prototypes
//   circleArea(r)            the area, or raises NegativeRadius
//   triangleArea(a, b, c)    the area (Heron), or raises DegenerateTriangle
//   parseRadius(text)        an integer parsed by protoCore: a malformed text
//                            raises protoCore's InvalidArgument error, built by
//                            the space's error factory
//   legacyArea(r)            the anti-pattern: throws std::runtime_error, the
//                            way C++ libraries do (see 04_foreign_cpp_exceptions)
//
// Every error value carries a `message` attribute.  Load it with
// `space.getImportModule(ctx, "example/geometry", "exports")` after
// registerGeometryProvider() and with "provider:example_geometry" in the
// space's resolution chain.
#ifndef PROTOCORE_EXAMPLE_GEOMETRY_MODULE_H
#define PROTOCORE_EXAMPLE_GEOMETRY_MODULE_H

namespace example {

/** Registers the "example_geometry" provider with protoCore's ProviderRegistry. */
void registerGeometryProvider();

}  // namespace example

#endif
