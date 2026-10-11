// embedder_exit -- an embedder that exits LeakSanitizer-clean.
//
// protoCore retains memory on purpose: interned symbols are perennial, and
// the cell heap is never returned to the system, not even when a space is
// destroyed (docs/MemoryModel.md section 1).  Retained is not leaked: every
// such allocation must stay reachable from a process-wide structure, so that
// a runtime can run its own suite under AddressSanitizer with leak detection
// and see only its own leaks.  Until 2.14.5 an embedder that did nothing but
// create a space reported leaks at exit:
//   * every new symbol left a second, unreferenced perennial copy of its
//     spelling (ProtoString::createSymbol built one, SymbolTable::intern
//     built another and kept that one);
//   * a destroyed space's heap blocks were referenced by nothing once the
//     space was gone, and its free-chunk descriptors were never deleted.
//
// Built as its own executable, not as a GoogleTest case: LeakSanitizer
// reports when the PROCESS exits, after every static destructor, and that is
// the moment an embedder sees.  The ctest entries set detect_leaks=1; in a
// build without AddressSanitizer they only run the scenarios (they certify
// nothing about leaks there).
//
// Modes:
//   alive      the space lives until the process exits (most runtimes);
//   destroyed  the space is destroyed before main returns;
//   respawn    several spaces created and destroyed in turn, two at once.

#include <protoCore.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace proto;

namespace {

const ProtoObject* worker(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                          const ProtoList*, const ProtoSparseList*) {
    for (int i = 0; i < 2000; ++i) {
        const ProtoObject* o = ctx->newObject(true);
        (void) o->setAttribute(ctx, ProtoString::createSymbol(ctx, "worker_attribute"),
                               ctx->fromInteger(i));
    }
    return PROTO_NONE;
}

// Symbols (new and repeated, short and long, ASCII and not), mutable and
// immutable objects, lists, strings, and a protoCore thread joined before
// returning.
bool exercise(ProtoSpace& space, int salt) {
    ProtoContext* ctx = space.rootContext;
    for (int i = 0; i < 200; ++i) {
        const std::string name = "embedder_exit_symbol_number_" + std::to_string(salt) +
                                 "_" + std::to_string(i % 50);
        const ProtoString* sym = ProtoString::createSymbol(ctx, name.c_str());
        if (sym != ProtoString::createSymbol(ctx, name.c_str())) return false;
    }
    (void) ProtoString::createSymbol(ctx, "s\xC3\xADmbolo_no_ascii_m\xC3\xA1s_largo_que_inline");
    const ProtoList* list = ctx->newList();
    for (int i = 0; i < 5000; ++i) {
        const ProtoObject* o = ctx->newObject(i % 2 == 0);
        (void) o->setAttribute(ctx, ProtoString::createSymbol(ctx, "embedder_exit_value"),
                               ctx->fromInteger(i));
        list = list->appendLast(ctx, o);
    }
    if (list->getSize(ctx) != 5000) return false;
    const ProtoThread* t = space.newThread(
        ctx, ProtoString::createSymbol(ctx, "embedder-exit-worker"), worker, nullptr, nullptr);
    const_cast<ProtoThread*>(t)->join(ctx);
    return true;
}

ProtoSpace* gAlive = nullptr;   // never destroyed: the "alive" mode

}  // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "alive";
    bool ok = true;
    if (std::strcmp(mode, "alive") == 0) {
        gAlive = new ProtoSpace();
        ok = exercise(*gAlive, 0);
    } else if (std::strcmp(mode, "destroyed") == 0) {
        ProtoSpace space;
        ok = exercise(space, 1);
    } else if (std::strcmp(mode, "respawn") == 0) {
        for (int round = 0; round < 3 && ok; ++round) {
            ProtoSpace a;
            ProtoSpace b;
            ok = exercise(a, 10 + round) && exercise(b, 20 + round);
        }
    } else {
        std::fprintf(stderr, "embedder_exit: unknown mode '%s'\n", mode);
        return 2;
    }
    std::printf("embedder_exit %s: %s\n", mode, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
