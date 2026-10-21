// 02_error_factory.cpp -- tutorial 02 (docs/tutorials/exceptions/).
//
// protoCore raises its own errors (a division by zero, a malformed integer
// literal) through the space's error factory.  Without one, they are
// instances of protoCore's own prototypes; a runtime installs a factory so
// they arrive as the language's exceptions, caught by the language's names.
#include "ExampleSupport.h"

using namespace proto;
using namespace example;

namespace {

// A toy language's exception hierarchy:
//     Exception
//      +-- ArithmeticError
//      |    +-- ZeroDivisionError
//      +-- ValueError
//      +-- TypeError
struct ToyLanguage {
    const ProtoObject* Exception;
    const ProtoObject* ArithmeticError;
    const ProtoObject* ZeroDivisionError;
    const ProtoObject* ValueError;
    const ProtoObject* TypeError;
};

// A factory is a plain function: a runtime keeps its classes where the
// factory can reach them (here a static, as a runtime would keep them in its
// environment) and pins them as GC roots (here a ProtoRootSet): a C++ pointer
// alone does not keep an object alive (EMBEDDER-CONFORMANCE.md, rule 3).
ToyLanguage g_lang;

const ProtoObject* named(ProtoContext* ctx, const ProtoObject* parent, const char* name) {
    const ProtoObject* o = parent ? parent->newChild(ctx) : ctx->newObject(false);
    return o->setAttribute(ctx, sym(ctx, "name"), ctx->fromUTF8String(name));
}

const ProtoObject* toyErrorFactory(ProtoContext* ctx, ProtoErrorKind kind, const char* message) {
    const ProtoObject* type = g_lang.Exception;
    switch (kind) {
        case ProtoErrorKind::DivisionByZero:  type = g_lang.ZeroDivisionError; break;
        case ProtoErrorKind::Overflow:        type = g_lang.ArithmeticError; break;
        case ProtoErrorKind::InvalidArgument: type = g_lang.ValueError; break;
        case ProtoErrorKind::TypeMismatch:
        case ProtoErrorKind::ArityMismatch:   type = g_lang.TypeError; break;
    }
    // Build the instance the way the language's own constructor would.
    return type->newChild(ctx)->setAttribute(ctx, sym(ctx, "message"), ctx->fromUTF8String(message));
}

const char* nameOf(ProtoContext* ctx, const ProtoObject* error) {
    static std::string s;
    s = error->getAttribute(ctx, sym(ctx, "name"))->asString(ctx)->toStdString(ctx);
    return s.c_str();
}

}  // namespace

int main() {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoObject* geometry = loadGeometry(space);

    // 1. No factory installed: protoCore's own prototypes.
    try {
        (void) ctx->fromInteger(7)->divide(ctx, ctx->fromInteger(0));
    } catch (const ProtoException& e) {
        check(e.isInstanceOf(ctx, space.getErrorPrototype(ctx, ProtoErrorKind::DivisionByZero)),
              "default factory: an instance of protoCore's DivisionByZero");
        check(e.isInstanceOf(ctx, space.getErrorRootPrototype(ctx)),
              "... which is a ProtoCoreError");
        std::printf("      %s: %s\n", nameOf(ctx, e.value()), messageOf(ctx, e.value()).c_str());
    }

    // 2. The language installs its factory once, at start-up.
    g_lang.Exception = named(ctx, nullptr, "Exception");
    g_lang.ArithmeticError = named(ctx, g_lang.Exception, "ArithmeticError");
    g_lang.ZeroDivisionError = named(ctx, g_lang.ArithmeticError, "ZeroDivisionError");
    g_lang.ValueError = named(ctx, g_lang.Exception, "ValueError");
    g_lang.TypeError = named(ctx, g_lang.Exception, "TypeError");
    ProtoRootSet* classes = space.createRootSet("toy-language-classes");
    for (const ProtoObject* c : {g_lang.Exception, g_lang.ArithmeticError, g_lang.ZeroDivisionError,
                                 g_lang.ValueError, g_lang.TypeError})
        classes->add(c);
    space.setErrorFactory(&toyErrorFactory);

    // 3. The same protoCore error is now the language's ZeroDivisionError,
    //    and a handler for ArithmeticError catches it.
    try {
        (void) ctx->fromInteger(7)->modulo(ctx, ctx->fromInteger(0));
    } catch (const ProtoException& e) {
        check(e.isInstanceOf(ctx, g_lang.ZeroDivisionError), "installed factory: a ZeroDivisionError");
        check(e.isInstanceOf(ctx, g_lang.ArithmeticError), "... caught as an ArithmeticError");
        std::printf("      %s: %s\n", nameOf(ctx, e.value()), messageOf(ctx, e.value()).c_str());
    }

    // 4. It holds inside a module too: parseRadius lets protoCore's error
    //    through, and the language sees its own ValueError.
    try {
        (void) callModule(ctx, geometry, "parseRadius", {ctx->fromUTF8String("ten")});
        check(false, "parseRadius(\"ten\") raises");
    } catch (const ProtoException& e) {
        check(e.isInstanceOf(ctx, g_lang.ValueError), "a malformed literal inside a module is a ValueError");
        std::printf("      %s: %s\n", nameOf(ctx, e.value()), messageOf(ctx, e.value()).c_str());
    }

    // 5. A wrong-typed operand is a TypeError.
    try {
        (void) ctx->fromUTF8String("abc")->asLong(ctx);
    } catch (const ProtoException& e) {
        check(e.isInstanceOf(ctx, g_lang.TypeError), "a string read as an integer is a TypeError");
    }

    space.setErrorFactory(nullptr);
    return finish("02_error_factory");
}
