// CaseExceptions.cpp -- rule 14: one exception type, selected by prototype.
//
// Every language on protoCore throws proto::ProtoException carrying the thrown
// value, and a handler selects by the value's prototype chain
// (docs/EXCEPTIONS.md).  That is what lets an error type defined by a module --
// in C++, or in another language -- be caught by name everywhere.  The kernel
// half of the rule is that protoCore raises its own errors that way and keeps
// the value alive in flight; the embedder half is that the language's handlers
// catch the base type, select by prototype, and let what they do not select
// leave unchanged.
#include "Cases.h"
#include "CycleDriver.h"

#include <exception>
#include <string>

namespace proto { namespace conformance {

namespace {

// The value the raising method throws.  The cases run one at a time on the
// host's main thread.
const ProtoObject* g_raised = nullptr;

const ProtoObject* raiseModuleError(ProtoContext* context, const ProtoObject*, const ParentLink*,
                                    const ProtoArguments*, const ProtoKeywordArguments*)
{
    context->raise(g_raised);
}

}  // namespace

// Kernel: protoCore's own failures are ProtoExceptions, built by the space's
// error factory, and never C++ standard exceptions.
CaseResult caseKernelErrorsAreProto(Host& host)
{
    const char* kId = "exception.kernel_errors_are_proto";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 14, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;
    const bool hostFactory = space.getErrorFactory() != nullptr;
    const std::string factory = hostFactory ? "the host's error factory"
                                            : "protoCore's default error factory";
    try {
        (void) ctx->fromInteger(1)->divide(ctx, ctx->fromInteger(0));
    } catch (const ProtoException& e) {
        if (!e.value() || e.value() == PROTO_NONE)
            return {kId, 14, Status::Fail, factory + " built no value for DivisionByZero"};
        if (!hostFactory
                && !e.isInstanceOf(ctx, space.getErrorPrototype(ctx, ProtoErrorKind::DivisionByZero)))
            return {kId, 14, Status::Fail,
                    "the default factory's value is not an instance of the DivisionByZero prototype"};
        return {kId, 14, Status::Pass,
                "1 / 0 raised a ProtoException built by " + factory};
    } catch (const std::exception& e) {
        return {kId, 14, Status::Fail,
                std::string("1 / 0 raised a C++ standard exception: ") + e.what()};
    } catch (...) {
        return {kId, 14, Status::Fail, "1 / 0 raised an exception of an unknown type"};
    }
    return {kId, 14, Status::Fail, "1 / 0 returned"};
}

// Kernel: the value of an exception in flight survives collections after the
// context that built it is gone.
CaseResult caseValuePinnedInFlight(Host& host)
{
    const char* kId = "exception.value_pinned_in_flight";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 14, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;
    const ProtoString* key = ProtoString::createSymbol(ctx, "conformance_exception_marker");

    std::exception_ptr inFlight;
    {
        ProtoContext child(&space, ctx);
        const ProtoObject* v = child.newObject(false)->setAttribute(&child, key, child.fromLong(0xE4C));
        try {
            child.raise(v);
        } catch (const ProtoException&) {
            inFlight = std::current_exception();
        }
    }
    host.makeGarbage(20000);
    CycleReport r = driveCycles(space, ctx, /*maxCycles=*/4, /*deadlineMs=*/20000);
    try {
        std::rethrow_exception(inFlight);
    } catch (const ProtoException& e) {
        const ProtoObject* marker = e.value()->getAttribute(ctx, key);
        if (!marker || marker == PROTO_NONE || marker->asLong(ctx) != 0xE4C)
            return {kId, 14, Status::Fail,
                    "the value of an exception in flight lost its contents after "
                    + std::to_string(r.cyclesRun) + " cycles.  " + describe(r, 0)};
    }
    return {kId, 14, Status::Pass,
            "the value survived " + std::to_string(r.cyclesRun) + " cycles.  " + describe(r, 0)};
}

// Embedder: the language's handler for a module's error type catches an
// instance of it, and lets an instance of an unrelated type leave unchanged.
CaseResult caseForeignTypeCaught(Host& host)
{
    const char* kId = "exception.foreign_type_caught";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 14, Status::Fail, "Host::mainContext() returned no usable context"};

    // A module's error hierarchy: ModuleError, and NotFound below it.
    const ProtoObject* moduleError = ctx->newObject(false);
    const ProtoObject* notFound = moduleError->newChild(ctx);
    const ProtoObject* unrelated = ctx->newObject(false);
    const ProtoObject* raising =
        ctx->fromMethod(nullptr, &raiseModuleError);

    // 1. A NotFound is caught by a handler for ModuleError, the very value.
    g_raised = notFound->newChild(ctx);
    const ProtoObject* caught = nullptr;
    int r = -1;
    try {
        r = host.catchForeignError(moduleError, raising, &caught);
    } catch (const ProtoException&) {
        g_raised = nullptr;
        return {kId, 14, Status::Fail,
                "the handler for the module's base type did not catch an instance of its subtype"};
    } catch (...) {
        g_raised = nullptr;
        return {kId, 14, Status::Fail,
                "the call raised something that is not a ProtoException"};
    }
    if (r == -1) {
        g_raised = nullptr;
        return unavailable(kId, 14, "Host::catchForeignError",
                           "the runtime does not run a handler around a module method");
    }
    if (caught != g_raised) {
        g_raised = nullptr;
        return {kId, 14, Status::Fail,
                "the handler ran but received a different value from the one raised"};
    }

    // 2. A handler for an unrelated type does not run; the exception leaves
    // the runtime with the same value.
    bool escaped = false;
    bool sameValue = false;
    try {
        caught = nullptr;
        (void) host.catchForeignError(unrelated, raising, &caught);
    } catch (const ProtoException& e) {
        escaped = true;
        sameValue = e.value() == g_raised;
    } catch (...) {
    }
    g_raised = nullptr;
    if (!escaped)
        return {kId, 14, Status::Fail,
                caught ? "a handler for an unrelated type caught the module's error"
                       : "the module's error did not leave the runtime as a ProtoException"};
    if (!sameValue)
        return {kId, 14, Status::Fail,
                "the module's error left the runtime with a different value"};
    return {kId, 14, Status::Pass,
            "caught by its base type with the same value; an unrelated handler let it pass"};
}

// Embedder: an unhandled language error leaves the runtime as a ProtoException.
CaseResult caseLanguageErrorEscapes(Host& host)
{
    const char* kId = "exception.language_error_escapes";
    try {
        (void) host.escapeLanguageError();
    } catch (const ProtoException& e) {
        if (!e.value() || e.value() == PROTO_NONE)
            return {kId, 14, Status::Fail, "the escaping ProtoException carries no value"};
        return {kId, 14, Status::Pass, "the language's error left the runtime as a ProtoException"};
    } catch (const std::exception& e) {
        return {kId, 14, Status::Fail,
                std::string("the language's error left the runtime as a C++ standard exception: ")
                + e.what()};
    } catch (...) {
        return {kId, 14, Status::Fail,
                "the language's error left the runtime as an exception of another C++ type"};
    }
    return unavailable(kId, 14, "Host::escapeLanguageError",
                       "the runtime does not raise an unhandled language error on request");
}

}}  // namespace proto::conformance
