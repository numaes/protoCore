// 03_unwinding_and_lifetime.cpp -- tutorial 03 (docs/tutorials/exceptions/).
//
// What happens between the raise and the handler:
//   - cleanup runs on the way out (a language `finally` / `ensure:` is a
//     catch-all that runs the cleanup and rethrows the same exception);
//   - the value stays alive while the exception is in flight, even when the
//     cleanup allocates and collections run;
//   - an exception can be kept and rethrown later (std::exception_ptr), the
//     way a future or a promise carries an error to another call;
//   - the pin is released with the last copy of the exception.
#include "ExampleSupport.h"

#include <chrono>
#include <exception>
#include <thread>

using namespace proto;
using namespace example;

namespace {

// Asks the collector for cycles until `n` more have completed.  Parked, so
// the collector can stop the world (EMBEDDER-CONFORMANCE.md, rule 2).
bool collect(ProtoSpace& space, ProtoContext* ctx, int n) {
    ProtoContext::UnmanagedScope parked(ctx);
    const uint64_t target = space.getGCCycleCount() + static_cast<uint64_t>(n);
    for (int i = 0; i < 30000 && space.getGCCycleCount() < target; ++i) {
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            space.gcStarted = true;
            space.gcCV.notify_all();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return space.getGCCycleCount() >= target;
}

// The C++ shape of `try { body } finally { cleanup }`.
template <typename Body, typename Cleanup>
void tryFinally(Body body, Cleanup cleanup) {
    try {
        body();
    } catch (...) {
        cleanup();
        throw;   // the same exception object, not a copy
    }
    cleanup();
}

}  // namespace

int main() {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoObject* geometry = loadGeometry(space);
    const ProtoObject* NegativeRadius = geometry->getAttribute(ctx, sym(ctx, "NegativeRadius"));

    // 1. finally: the cleanup runs, allocates, and the handler still sees the
    //    value intact.
    bool cleaned = false;
    try {
        tryFinally(
            [&] { (void) callModule(ctx, geometry, "circleArea", {ctx->fromInteger(-3)}); },
            [&] {
                // Cleanup that allocates, in its own context, and collects.
                for (int i = 0; i < 5000; ++i) {
                    ProtoContext scratch(&space, ctx);
                    (void) scratch.newObject(false)->setAttribute(&scratch, sym(&scratch, "i"),
                                                                 scratch.fromInteger(i));
                }
                cleaned = collect(space, ctx, 2);
            });
    } catch (const ProtoException& e) {
        check(cleaned, "the cleanup ran, and two collections completed, before the handler");
        check(e.isInstanceOf(ctx, NegativeRadius), "the handler received the NegativeRadius");
        check(messageOf(ctx, e.value()).find("is negative") != std::string::npos,
              "its message survived the collections during unwinding");
    }

    // 2. Keep the exception, drop every other reference, collect, rethrow.
    std::exception_ptr kept;
    {
        ProtoContext call(&space, ctx);
        try {
            (void) callModule(&call, geometry, "circleArea", {call.fromInteger(-4)});
        } catch (const ProtoException&) {
            kept = std::current_exception();
        }
    }   // the context that built the value is gone
    check(space.exceptionRoots_->size() == 1, "one value is pinned while the exception is kept");
    check(collect(space, ctx, 2), "two collections completed while the exception was kept");
    try {
        std::rethrow_exception(kept);
    } catch (const ProtoException& e) {
        check(e.isInstanceOf(ctx, NegativeRadius), "the kept exception rethrows with its value");
        check(messageOf(ctx, e.value()).find("-4") != std::string::npos, "... intact after collections");
    }

    // 3. The last copy releases the pin.
    kept = nullptr;
    check(space.exceptionRoots_->size() == 0, "no value is pinned once the exception is gone");

    return finish("03_unwinding_and_lifetime");
}
