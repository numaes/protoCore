// CaseModules.cpp -- the post-P3 rules 9b and 9c.
#include "Cases.h"
#include "CycleDriver.h"

#include <string>

namespace proto { namespace conformance {

// Rule 9b -- the module list is a GC root.
//
// P3's central distinction, and the thing this case exists to keep true:
// NEVER FREED IS NOT THE SAME AS IS A GC ROOT.  A perennial cell is not swept,
// but it is also not SCANNED, so the references it holds do not keep their
// targets alive.  A symbol gets away with perennial allocation alone because it
// is self-contained bytes.  A module object's contents are ordinary collectable
// objects in a space's heap, so the list must be a real root the mark enters
// through.
//
// The case: register a module through the kernel, drop every other reference to
// it, drive cycles, then read an attribute back out of it.  A module whose list
// is merely unfreed fails here and passes every "does it leak" test.
CaseResult caseModuleRootSurvivesCycle(Host& host)
{
    const char* kId = "module.root_survives_cycle";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 9, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;

    const unsigned long rootsBefore = ProtoSpace::moduleRootCount();
    const ModuleIdentity id("protoCore.conformance",
                            "conformance/module_root_survives_cycle", "1.0.0");

    const ProtoString* key = ProtoString::createSymbol(ctx, "conformance_module_marker");
    const ProtoObject* published = nullptr;
    {
        // Build the module in a child context and let the context die, so the
        // module's own cells are no longer anchored by any young chain.  Only the
        // module root table can keep them alive from here.
        ProtoContext child(&space, ctx);
        const ProtoObject* module = child.newObject(/*mutableObject=*/false);
        module = module->setAttribute(&child, key, child.fromLong(0xB0D1E5));
        published = space.registerModule(id, module);
    }
    if (!published || published == PROTO_NONE)
        return {kId, 9, Status::Fail,
                "ProtoSpace::registerModule did not publish the module, so rule "
                "9b cannot be observed"};

    const unsigned long rootsAfter = ProtoSpace::moduleRootCount();

    // Drop our own reference.  `published` stays as a C++ local, which is NOT a
    // GC root -- so if the module root table is not a real root, the mark will
    // not reach the module and the sweep will free it under us.  That is the
    // point of the case, and it is also why it must re-fetch through findModule
    // rather than trusting the local.
    published = nullptr;

    CycleReport r = driveCycles(space, ctx, /*maxCycles=*/6, /*deadlineMs=*/20000);

    const ProtoObject* found = ProtoSpace::findModule(id);
    const std::string common =
        "moduleRootCount " + std::to_string(rootsBefore) + "->"
        + std::to_string(rootsAfter) + "; " + describe(r, 0);

    if (!found || found == PROTO_NONE)
        return {kId, 9, Status::Fail,
                "the module was registered and then could not be found after "
                + std::to_string(r.cyclesRun) + " collection cycles.  " + common};

    const ProtoObject* marker = found->getAttribute(ctx, key);
    if (marker == nullptr || marker == PROTO_NONE)
        return {kId, 9, Status::Fail,
                "the module survived but its contents did not: the marker "
                "attribute is gone after " + std::to_string(r.cyclesRun)
                + " cycles.  That is exactly the 'never freed is not a root' "
                  "failure -- the list survived and its contents were collected "
                  "under it.  " + common};

    return {kId, 9, Status::Pass,
            "a registered module and its contents survived "
            + std::to_string(r.cyclesRun)
            + " collection cycles with no other reference to them, so the module "
              "list is a real GC root and not merely unfreed memory.  " + common};
}

// Rule 9c -- a module's identity is provider + path + version.
//
// Keying by path alone aliases two different modules into one, first load
// winning: a wrong answer with no error, which is the same failure class as the
// 6-versus-7-byte interning bug.
//
// The kernel half is checked here directly, because it needs no runtime: two
// identities that differ only in provider must not resolve to the same module.
// The runtime half needs the embedder's own provider abstraction and is asked
// for through loadSamePathTwoProviders().
CaseResult caseModuleAliasRejected(Host& host)
{
    const char* kId = "module.alias_rejected";
    ProtoContext* ctx = host.mainContext();
    if (!ctx || !ctx->space)
        return {kId, 9, Status::Fail, "Host::mainContext() returned no usable context"};
    ProtoSpace& space = *ctx->space;

    const ModuleIdentity a("provider.A", "conformance/same_path", "1.0.0");
    const ModuleIdentity b("provider.B", "conformance/same_path", "1.0.0");
    const ModuleIdentity aNoVersion =
        ModuleIdentity::unversioned("provider.A", "conformance/same_path");

    const ProtoObject* modA = space.registerModule(a, ctx->newObject(false));
    const ProtoObject* modB = space.registerModule(b, ctx->newObject(false));
    const ProtoObject* modAnv = space.registerModule(aNoVersion, ctx->newObject(false));

    const std::string common =
        "keys: A='" + a.asKey() + "' B='" + b.asKey() + "' A-unversioned='"
        + aNoVersion.asKey() + "'";

    if (!modA || !modB || !modAnv)
        return {kId, 9, Status::Fail,
                "registerModule did not publish one of the three identities.  "
                + common};

    if (modA == modB)
        return {kId, 9, Status::Fail,
                "two identities differing only in PROVIDER resolved to the same "
                "module, so the registry is keyed by path alone: two different "
                "modules alias into one and the first load wins.  " + common};

    if (modA == modAnv)
        return {kId, 9, Status::Fail,
                "an identity with version '1.0.0' and one that declares no "
                "version resolved to the same module, so the version is not part "
                "of the key.  The empty version is reserved for 'declares none' "
                "and is a distinct identity.  " + common};

    const int hostAnswer = host.loadSamePathTwoProviders();
    if (hostAnswer == 0)
        return {kId, 9, Status::Fail,
                "the kernel keeps provider+path+version distinct, but THIS "
                "RUNTIME aliased the same logical path loaded from two different "
                "providers into one module.  The kernel offers the right key; "
                "the runtime is not using it.  " + common};

    if (hostAnswer < 0)
        return {kId, 9, Status::NeedsReview,
                "the kernel keeps provider, path and version distinct.  The "
                "runtime half is unchecked: Host::loadSamePathTwoProviders() is "
                "unavailable, so this runtime's own module keying is NOT covered "
                "by this result -- implement the capability or answer it in "
                "docs/CONFORMANCE.md.  " + common};

    return {kId, 9, Status::Pass,
            "the kernel and the runtime both keep the same logical path from two "
            "providers distinct.  " + common};
}

}}  // namespace proto::conformance
