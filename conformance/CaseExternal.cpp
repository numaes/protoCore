// CaseExternal.cpp -- rule 7's runtime half.
#include "Cases.h"
#include "CycleDriver.h"

#include <atomic>
#include <string>

namespace proto { namespace conformance {

namespace {

std::atomic<int> g_finalized{0};
std::atomic<int> g_payload{0};

/// The finalizer obeys the contract it is testing: it increments a
/// std::atomic<int> and does nothing else.  It does not allocate, does not
/// touch a ProtoObject*, does not CAS into a shared structure and cannot block
/// (docs/GarbageCollector.md section 7).
void conformingFinalizer(void* p)
{
    if (p) g_payload.store(*static_cast<int*>(p));
    g_finalized.fetch_add(1);
    delete static_cast<int*>(p);
}

}  // namespace

// Rule 7 (runtime half) -- an external wrapper's finalizer runs when the wrapper
// is collected, and that is the premise every embedder's external-memory
// bookkeeping rests on.  protoCore has no test for it, so a change to the sweep
// that stopped running finalizers would land silently under five runtimes at
// once.  This is a CHARACTERISATION case: it asserts the documented behaviour so
// that a change to it cannot be silent.
//
// It deliberately does NOT assert the perennial half of the contract
// (MemoryModel.md section 5: "A perennial wrapper is never swept, so its
// finalizer never runs; and nothing runs finalizers at process exit").
// Constructing a perennial wrapper requires reaching protoCore's contextless
// allocation path, which is not part of the public API, and a case that faked it
// would be asserting its own expectation rather than the kernel's behaviour.
// The perennial trap is documented in EMBEDDER-CONFORMANCE.md rule 7 and
// answered in checklist C7; it is honest to say so rather than to ship a case
// that cannot mean what its name claims.
CaseResult casePerennialNeverFinalized(Host& host)
{
    const char* kId = "external.finalizer_runs";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 7, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;

    g_finalized.store(0);
    g_payload.store(0);

    {
        // A child context so the wrapper's cell leaves the young chain when the
        // context is destroyed, rather than depending on the main context ever
        // reaching its submission threshold.
        ProtoContext child(&space, ctx);
        int* payload = new int(0x5EED);
        const ProtoObject* wrapper = child.fromExternalPointer(payload, &conformingFinalizer);
        if (!wrapper || wrapper == PROTO_NONE) {
            delete payload;
            return {kId, 7, Status::Fail,
                    "ProtoContext::fromExternalPointer returned no wrapper, so "
                    "rule 7's premise cannot be observed at all"};
        }
        // The wrapper is dropped here: nothing references it once `child` dies.
    }

    CycleReport r = driveCycles(space, ctx, /*maxCycles=*/8, /*deadlineMs=*/20000);

    const std::string common =
        "finalizerCalls=" + std::to_string(g_finalized.load())
        + " payloadSeen=0x" + [] { char b[16]; std::snprintf(b, sizeof b, "%X", g_payload.load()); return std::string(b); }()
        + "; " + describe(r, 0);

    if (g_finalized.load() == 0)
        return {kId, 7, Status::Fail,
                "the wrapper was dropped and " + std::to_string(r.cyclesRun)
                + " collection cycles ran, but its finalizer never fired, so "
                  "every byte of external memory this runtime hands to protoCore "
                  "leaks.  Before reporting a kernel bug, raise the deadline: "
                  "MemoryModel.md section 5 gives no promptness guarantee, so a "
                  "slow sweep and a broken contract look the same from here, and "
                  "a case that cannot tell them apart would be worse than none.  "
                + common};

    if (g_finalized.load() != 1)
        return {kId, 7, Status::Fail,
                "the finalizer fired " + std::to_string(g_finalized.load())
                + " times for one wrapper.  A finalizer that runs more than once "
                  "double-frees whatever it owns.  " + common};

    if (g_payload.load() != 0x5EED)
        return {kId, 7, Status::Fail,
                "the finalizer ran but received the wrong pointer.  " + common};

    return {kId, 7, Status::Pass,
            "a dropped external wrapper's finalizer ran exactly once, with the "
            "pointer it was given.  " + common};
}

// The accounting probe, honest about what it can and cannot see.
//
// MemoryModel.md section 5 draws the boundary and explains why it cannot be
// moved: "External size is whatever the embedder declares. ... A collection
// policy driven by that number is a policy driven by a figure that can drift
// arbitrarily from reality, and the kernel has no way to detect the drift."
//
// A conformance suite is in exactly the same position.  So this probe answers
// only the question it CAN answer -- does an accounting exist at all -- and
// hands the rest to checklist C7.
CaseResult caseExternalBytesAccounted(Host& host)
{
    const char* kId = "external.bytes_accounted";
    const unsigned long bytes = host.externalBytesAccounted();
    if (bytes == (unsigned long) -1)
        return {kId, 7, Status::NeedsReview,
                "this runtime keeps no total of the memory it allocates outside "
                "protoCore.  MemoryModel.md section 5 assigns that total to the "
                "embedder ('Count it'), because protoCore cannot see it: "
                "external bytes do not count toward heapSize, create no "
                "collection pressure and are not in the traced graph.  Whether "
                "this runtime needs a total is checklist C7 -- a runtime with no "
                "external wrappers at all answers it in one line -- and it is "
                "not something this case can decide"};
    return {kId, 7, Status::Pass,
            "the runtime reports " + std::to_string(bytes)
            + " bytes of externally-allocated memory under its own accounting.  "
              "Whether that number is RIGHT -- double counting through two "
              "wrappers, a region freed elsewhere -- is checklist C7 and is "
              "unknowable to protoCore by construction"};
}

}}  // namespace proto::conformance
