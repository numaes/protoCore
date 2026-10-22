// GeometryModule.cpp -- see GeometryModule.h.
//
// The module never names a language.  It raises a ProtoException whose value
// is an instance of its own error prototype; whichever language called it
// selects the error by that prototype (docs/EXCEPTIONS.md).
#include "GeometryModule.h"
#include "protoCore.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>

namespace example {

namespace {

using namespace proto;

const ProtoString* sym(ProtoContext* ctx, const char* name) {
    return ProtoString::createSymbol(ctx, name);
}

// Raises an instance of the module's error type `typeName` with `message`.
// `module` is the receiver of the call: the module object, which holds the
// error prototypes as attributes.
[[noreturn]] void raiseGeometryError(ProtoContext* ctx, const ProtoObject* module,
                                     const char* typeName, const std::string& message) {
    const ProtoObject* type = module->getAttribute(ctx, sym(ctx, typeName));
    const ProtoObject* error = type->newChild(ctx)->setAttribute(
        ctx, sym(ctx, "message"), ctx->fromUTF8String(message.c_str()));
    ctx->raise(error);
}

double number(ProtoContext* ctx, const ProtoArguments* args, int i) {
    const ProtoObject* v = args ? args->getAt(ctx, i) : PROTO_NONE;
    if (v->isDouble(ctx)) return v->asDouble(ctx);
    // protoCore raises TypeMismatch for a value that is not an integer.
    return static_cast<double>(v->asLong(ctx));
}

const ProtoObject* circleArea(ProtoContext* ctx, const ProtoObject* self, const ParentLink*,
                              const ProtoArguments* args, const ProtoKeywordArguments*) {
    const double r = number(ctx, args, 0);
    if (r < 0)
        raiseGeometryError(ctx, self, "NegativeRadius",
                           "circleArea: the radius " + std::to_string(r) + " is negative");
    return ctx->fromDouble(std::numbers::pi * r * r);
}

const ProtoObject* triangleArea(ProtoContext* ctx, const ProtoObject* self, const ParentLink*,
                                const ProtoArguments* args, const ProtoKeywordArguments*) {
    const double a = number(ctx, args, 0), b = number(ctx, args, 1), c = number(ctx, args, 2);
    if (a + b <= c || a + c <= b || b + c <= a)
        raiseGeometryError(ctx, self, "DegenerateTriangle",
                           "triangleArea: the sides do not make a triangle");
    const double s = (a + b + c) / 2;
    return ctx->fromDouble(std::sqrt(s * (s - a) * (s - b) * (s - c)));
}

const ProtoObject* parseRadius(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                               const ProtoArguments* args, const ProtoKeywordArguments*) {
    const std::string text = args->getAt(ctx, 0)->asString(ctx)->toStdString(ctx);
    // A malformed literal: protoCore raises InvalidArgument through the
    // space's error factory.  The module does not catch it.
    return ctx->fromString(text.c_str(), 10);
}

const ProtoObject* legacyArea(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                              const ProtoArguments* args, const ProtoKeywordArguments*) {
    const double r = number(ctx, args, 0);
    // The anti-pattern: a C++ standard exception is not a language value.
    if (r < 0) throw std::runtime_error("legacyArea: negative radius");
    return ctx->fromDouble(std::numbers::pi * r * r);
}

class GeometryProvider final : public ModuleProvider {
public:
    const ProtoObject* tryLoad(const std::string& logicalPath, ProtoContext* ctx) override {
        if (logicalPath != "example/geometry") return PROTO_NONE;

        // The error hierarchy: two subtypes of the module's base type.  The
        // `name` attribute is for messages; selection uses the prototypes.
        const ProtoObject* base = ctx->newObject(false)->setAttribute(
            ctx, sym(ctx, "name"), ctx->fromUTF8String("GeometryError"));
        const ProtoObject* negative = base->newChild(ctx)->setAttribute(
            ctx, sym(ctx, "name"), ctx->fromUTF8String("NegativeRadius"));
        const ProtoObject* degenerate = base->newChild(ctx)->setAttribute(
            ctx, sym(ctx, "name"), ctx->fromUTF8String("DegenerateTriangle"));

        const ProtoObject* m = ctx->newObject(false);
        m = m->setAttribute(ctx, sym(ctx, "GeometryError"), base);
        m = m->setAttribute(ctx, sym(ctx, "NegativeRadius"), negative);
        m = m->setAttribute(ctx, sym(ctx, "DegenerateTriangle"), degenerate);
        m = m->setAttribute(ctx, sym(ctx, "circleArea"), ctx->fromMethod(nullptr, &circleArea));
        m = m->setAttribute(ctx, sym(ctx, "triangleArea"), ctx->fromMethod(nullptr, &triangleArea));
        m = m->setAttribute(ctx, sym(ctx, "parseRadius"), ctx->fromMethod(nullptr, &parseRadius));
        m = m->setAttribute(ctx, sym(ctx, "legacyArea"), ctx->fromMethod(nullptr, &legacyArea));
        return m;
    }
    const std::string& getGUID() const override { return guid_; }
    const std::string& getAlias() const override { return alias_; }

private:
    std::string guid_ = "protoCore.example.geometry.1";
    std::string alias_ = "example_geometry";
};

}  // namespace

void registerGeometryProvider() {
    ProviderRegistry::instance().registerProvider(std::make_unique<GeometryProvider>());
}

}  // namespace example
