/*
 * NonInternedNames.cpp -- counts of attribute operations that received a
 * non-interned name (ProtoSpace::nonInternedNameCounts).
 *
 * An attribute key is the address of an interned symbol, so a name passed as
 * a plain heap string is canonicalised on every call: a content hash and a
 * walk of the symbol table.  The counters say how often each operation does
 * it.  They are touched only on that path, which is already the slow one: one
 * relaxed increment.
 *
 * PROTOCORE_NONINTERNED_NAMES adds where the calls come from.  Each call then
 * also records its caller's return address in a fixed lock-free table (4,096
 * addresses; later ones are counted as "other"), and at process exit the
 * counts and the 20 most frequent callers are written out, symbolised with
 * dladdr where the platform has it.  "1" or "stderr" writes to stderr; any
 * other value is a file path the report is appended to, one block per
 * process.
 */

#include "proto_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#include <unistd.h>
#define PROTO_HAVE_DLADDR 1
#endif

namespace proto {
namespace nonInterned {

namespace {

std::atomic<proto_ulong> g_counts[kKinds];

constexpr std::size_t kSites = 4096;
struct Site {
    std::atomic<uintptr_t> caller{0};
    std::atomic<proto_ulong> count[kKinds];
};
Site* g_sites = nullptr;                 // allocated once when reporting is on
std::atomic<proto_ulong> g_otherSites{0};

const char* kindName(int k) {
    static const char* names[kKinds] = {"get", "has", "hasOwn", "set", "setBulk", "setIfEqual", "remove"};
    return names[k];
}

const char* reportDestination() {
    static const char* dest = std::getenv("PROTOCORE_NONINTERNED_NAMES");
    return dest;
}

void writeReport() {
    const char* dest = reportDestination();
    if (!dest || !g_sites) return;
    const bool toStderr = !std::strcmp(dest, "1") || !std::strcmp(dest, "stderr");
    std::FILE* out = toStderr ? stderr : std::fopen(dest, "a");
    if (!out) return;
    proto_ulong total = 0;
    for (int k = 0; k < kKinds; ++k) total += g_counts[k].load(std::memory_order_relaxed);
#if defined(PROTO_HAVE_DLADDR)
    std::fprintf(out, "protoCore PROTOCORE_NONINTERNED_NAMES [pid %ld]: total=%llu",
                 static_cast<long>(::getpid()), static_cast<unsigned long long>(total));
#else
    std::fprintf(out, "protoCore PROTOCORE_NONINTERNED_NAMES: total=%llu",
                 static_cast<unsigned long long>(total));
#endif
    for (int k = 0; k < kKinds; ++k)
        std::fprintf(out, " %s=%llu", kindName(k),
                     static_cast<unsigned long long>(g_counts[k].load(std::memory_order_relaxed)));
    std::fprintf(out, " untracked_sites=%llu\n",
                 static_cast<unsigned long long>(g_otherSites.load(std::memory_order_relaxed)));

    struct Row { uintptr_t caller; proto_ulong total; proto_ulong byKind[kKinds]; };
    std::vector<Row> rows;
    for (std::size_t i = 0; i < kSites; ++i) {
        const uintptr_t c = g_sites[i].caller.load(std::memory_order_relaxed);
        if (!c) continue;
        Row r{c, 0, {}};
        for (int k = 0; k < kKinds; ++k) {
            r.byKind[k] = g_sites[i].count[k].load(std::memory_order_relaxed);
            r.total += r.byKind[k];
        }
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.total > b.total; });
    const std::size_t shown = std::min<std::size_t>(rows.size(), 20);
    for (std::size_t i = 0; i < shown; ++i) {
        const Row& r = rows[i];
        std::fprintf(out, "  %10llu  ", static_cast<unsigned long long>(r.total));
        for (int k = 0; k < kKinds; ++k)
            if (r.byKind[k]) std::fprintf(out, "%s=%llu ", kindName(k), static_cast<unsigned long long>(r.byKind[k]));
#if defined(PROTO_HAVE_DLADDR)
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(r.caller), &info) && info.dli_fname) {
            const char* file = std::strrchr(info.dli_fname, '/');
            std::fprintf(out, " %s", file ? file + 1 : info.dli_fname);
            if (info.dli_sname)
                std::fprintf(out, " %s+0x%llx", info.dli_sname,
                             static_cast<unsigned long long>(r.caller - reinterpret_cast<uintptr_t>(info.dli_saddr)));
            else
                std::fprintf(out, " +0x%llx", static_cast<unsigned long long>(
                                 r.caller - reinterpret_cast<uintptr_t>(info.dli_fbase)));
            std::fputc('\n', out);
            continue;
        }
#endif
        std::fprintf(out, " 0x%llx\n", static_cast<unsigned long long>(r.caller));
    }
    std::fflush(out);
    if (!toStderr) std::fclose(out);
}

Site* sitesTable() {
    // Allocated and registered once, the first time a site is recorded.  Never
    // freed: the exit handler reads it.
    static Site* table = [] {
        Site* t = new Site[kSites];
        for (std::size_t i = 0; i < kSites; ++i)
            for (auto& c : t[i].count) c.store(0, std::memory_order_relaxed);
        g_sites = t;
        std::atexit(writeReport);
        return t;
    }();
    return table;
}

void recordSite(Kind kind, const void* caller) {
    Site* table = sitesTable();
    const uintptr_t c = reinterpret_cast<uintptr_t>(caller);
    std::size_t i = static_cast<std::size_t>((c >> 2) * 0x9E3779B97F4A7C15ULL >> 52) & (kSites - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kSites - 1)) {
        uintptr_t seen = table[i].caller.load(std::memory_order_relaxed);
        // An empty slot is claimed; if another thread claimed it first, the
        // failed exchange leaves that thread's caller in `seen`.
        if (seen == 0 && table[i].caller.compare_exchange_strong(seen, c, std::memory_order_relaxed))
            seen = c;
        if (seen == c) {
            table[i].count[kind].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_otherSites.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void note(Kind kind, const void* caller) {
    g_counts[kind].fetch_add(1, std::memory_order_relaxed);
    if (reportDestination()) recordSite(kind, caller);
}

}  // namespace nonInterned

NonInternedNameCounts ProtoSpace::nonInternedNameCounts() {
    using namespace nonInterned;
    NonInternedNameCounts c;
    c.get        = g_counts[Get].load(std::memory_order_relaxed);
    c.has        = g_counts[Has].load(std::memory_order_relaxed);
    c.hasOwn     = g_counts[HasOwn].load(std::memory_order_relaxed);
    c.set        = g_counts[Set].load(std::memory_order_relaxed);
    c.setBulk    = g_counts[SetBulk].load(std::memory_order_relaxed);
    c.setIfEqual = g_counts[SetIfEqual].load(std::memory_order_relaxed);
    c.remove     = g_counts[Remove].load(std::memory_order_relaxed);
    return c;
}

}  // namespace proto
