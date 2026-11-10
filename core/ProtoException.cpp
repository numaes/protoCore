/*
 * ProtoException.cpp
 *
 * The one C++ exception of every language on protoCore, and protoCore's own
 * errors.  See ProtoException and ProtoErrorKind in protoCore.h and
 * docs/EXCEPTIONS.md for the model.
 *
 * Implementation outline:
 *
 *   - A ProtoException pins its value in the space's
 *     "protocore-exceptions-in-flight" root set (created with the space).
 *     The pin is shared by every copy of the exception (a throw copies it;
 *     std::exception_ptr keeps one more), and the last copy removes it.
 *     Tagged values (SmallInteger, booleans, PROTO_NONE, ...) are never
 *     collected and are not pinned.
 *
 *   - protoCore's error prototypes are built once per space, the first time
 *     an error is raised with the default factory or a runtime asks for
 *     them, and pinned in their own root set for the space's lifetime.
 */

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include <mutex>
#include <string>

namespace proto {

// ---- ProtoException ---------------------------------------------------

struct ProtoException::Pin {
    Pin(ProtoRootSet* s, const ProtoObject* v) : set(s), handle(s->add(v)) {}
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
    ~Pin() { set->remove(handle); }
    ProtoRootSet* set;
    ProtoRootSet::Handle handle;
};

ProtoException::ProtoException(ProtoContext* context, const ProtoObject* value)
    : value_(value ? value : PROTO_NONE) {
    if (context && context->space && context->space->exceptionRoots_
            && value_->isCell(context)) {
        pin_ = std::make_shared<Pin>(context->space->exceptionRoots_, value_);
    }
}

// Out of line on purpose: the key function makes libprotoCore the one place
// the vtable and typeinfo are emitted, so a catch in one shared library
// matches a throw from another.
ProtoException::~ProtoException() = default;

bool ProtoException::isInstanceOf(ProtoContext* context, const ProtoObject* type) const {
    if (!type || type == PROTO_NONE) return false;
    return value_->isInstanceOf(context, type) == PROTO_TRUE;
}

// ---- Raising ----------------------------------------------------------

void ProtoContext::raise(const ProtoObject* value) {
    throw ProtoException(this, value);
}

namespace {

// The default factory: a child of protoCore's prototype for the kind, with
// the message as its `message` attribute.
const ProtoObject* defaultErrorFactory(ProtoContext* context, ProtoErrorKind kind,
                                       const std::string& message) {
    const ProtoObject* error = context->space->getErrorPrototype(context, kind)->newChild(context);
    return error->setAttribute(context, ProtoString::createSymbol(context, "message"),
                               context->fromUTF8String(message));
}

// Builds the error value for `message` (every byte of it) with the space's
// factory: the length-aware one when installed, else the zero-terminated one
// (which sees the message up to its first NUL), else the default.
const ProtoObject* buildErrorValue(ProtoContext* context, ProtoErrorKind kind,
                                   const std::string& message) {
    ProtoSpace* space = context->space;
    if (space) {
        if (ProtoErrorFactoryWithLength f = space->getErrorFactoryWithLength())
            return f(context, kind, message.c_str(), static_cast<proto_ulong>(message.size()));
        if (ProtoErrorFactory f = space->getErrorFactory())
            return f(context, kind, message.c_str());
    }
    return defaultErrorFactory(context, kind, message);
}

}  // namespace

void ProtoContext::raiseError(ProtoErrorKind kind, const char* message) {
    const ProtoObject* value = buildErrorValue(this, kind, std::string(message ? message : ""));
    throw ProtoException(this, value);
}

void ProtoContext::raiseError(ProtoErrorKind kind, const std::string& message) {
    const ProtoObject* value = buildErrorValue(this, kind, message);
    throw ProtoException(this, value);
}

// ---- The space's error state -------------------------------------------

void ProtoSpace::setErrorFactory(ProtoErrorFactory factory) {
    errorFactory_.store(factory, std::memory_order_release);
}

ProtoErrorFactory ProtoSpace::getErrorFactory() const {
    return errorFactory_.load(std::memory_order_acquire);
}

void ProtoSpace::setErrorFactoryWithLength(ProtoErrorFactoryWithLength factory) {
    if (impl) impl->errorFactoryWithLength.store(factory, std::memory_order_release);
}

ProtoErrorFactoryWithLength ProtoSpace::getErrorFactoryWithLength() const {
    return impl ? impl->errorFactoryWithLength.load(std::memory_order_acquire) : nullptr;
}

const char* ProtoSpace::errorKindName(ProtoErrorKind kind) {
    switch (kind) {
        case ProtoErrorKind::InvalidArgument: return "InvalidArgument";
        case ProtoErrorKind::TypeMismatch:    return "TypeMismatch";
        case ProtoErrorKind::Overflow:        return "Overflow";
        case ProtoErrorKind::DivisionByZero:  return "DivisionByZero";
        case ProtoErrorKind::ArityMismatch:   return "ArityMismatch";
    }
    return "ProtoCoreError";
}

namespace {

void buildErrorPrototypes(ProtoSpace* space, ProtoContext* context) {
    const ProtoString* nameKey = ProtoString::createSymbol(context, "name");
    ProtoRootSet* roots = space->createRootSet("protocore-error-prototypes");
    const ProtoObject* root = context->newObject(false)->setAttribute(
        context, nameKey, context->fromUTF8String("ProtoCoreError"));
    roots->add(root);
    space->errorPrototypes_[kProtoErrorKindCount] = root;
    for (int k = 0; k < kProtoErrorKindCount; ++k) {
        const char* name = ProtoSpace::errorKindName(static_cast<ProtoErrorKind>(k));
        const ProtoObject* proto = root->newChild(context)->setAttribute(
            context, nameKey, context->fromUTF8String(name));
        roots->add(proto);
        space->errorPrototypes_[k] = proto;
    }
    space->errorRoots_ = roots;
}

}  // namespace

const ProtoObject* ProtoSpace::getErrorRootPrototype(ProtoContext* context) {
    std::call_once(errorPrototypesOnce_, buildErrorPrototypes, this, context);
    return errorPrototypes_[kProtoErrorKindCount];
}

const ProtoObject* ProtoSpace::getErrorPrototype(ProtoContext* context, ProtoErrorKind kind) {
    std::call_once(errorPrototypesOnce_, buildErrorPrototypes, this, context);
    const int k = static_cast<int>(kind);
    if (k < 0 || k >= kProtoErrorKindCount) return errorPrototypes_[kProtoErrorKindCount];
    return errorPrototypes_[k];
}

}  // namespace proto
