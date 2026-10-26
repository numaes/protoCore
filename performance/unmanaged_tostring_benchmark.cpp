/*
 * unmanaged_tostring_benchmark.cpp -- two per-call costs embedders pay on
 * every line of a line-oriented workload (protoClojure's grep, 2026-10-25):
 *
 *   1. an empty ProtoContext::UnmanagedScope (enter + leave), single thread
 *      and with several threads at once;
 *   2. ProtoString::toStdString on a 79-byte ASCII line, a non-ASCII line,
 *      a short inline string and a 64 KiB rope.
 *
 * Self-reporting: every section prints the work done and a checksum of the
 * produced bytes, and the program exits non-zero when a check fails, so a
 * crash or a wrong result cannot pass for a fast run.  Uses the public API
 * only: one binary measures any ABI-compatible libprotoCore.so.
 *
 * Usage: unmanaged_tostring_benchmark [scale]   (default 1; iterations x scale)
 */

#include "../headers/protoCore.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace proto;

namespace {

double cpuSeconds() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec +
           (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

double sysSeconds() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

uint64_t fnv(const std::string& s, uint64_t h) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
    return h;
}

int failures = 0;

void scopeSingle(ProtoSpace& space, long n) {
    ProtoContext* ctx = space.rootContext;
    const double cpu0 = cpuSeconds(), sys0 = sysSeconds();
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < n; ++i) {
        ProtoContext::UnmanagedScope u(ctx);
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("scope_1thread      : %ld scopes  %.1f ns/scope  cpu/wall %.2f  sys %.2f s\n",
                n, s * 1e9 / n, (cpuSeconds() - cpu0) / s, sysSeconds() - sys0);
}

struct MtShared {
    long perThread = 0;
    std::atomic<long> done{0};
};
MtShared* gMt = nullptr;

const ProtoObject* scopeWorker(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                               const ProtoArguments*, const ProtoKeywordArguments*) {
    long k = 0;
    for (long i = 0; i < gMt->perThread; ++i) {
        ProtoContext::UnmanagedScope u(ctx);
        ++k;
    }
    gMt->done.fetch_add(k);
    return PROTO_NONE;
}

void scopeThreads(ProtoSpace& space, int threads, long perThread) {
    MtShared shared;
    shared.perThread = perThread;
    gMt = &shared;
    const double cpu0 = cpuSeconds(), sys0 = sysSeconds();
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<const ProtoThread*> ts;
    for (int t = 0; t < threads; ++t)
        ts.push_back(space.newThread(space.rootContext,
                                     ProtoString::fromUTF8(space.rootContext, "scope-worker"),
                                     scopeWorker, nullptr, nullptr));
    for (const ProtoThread* t : ts) const_cast<ProtoThread*>(t)->join(space.rootContext);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const long total = shared.done.load();
    if (total != threads * perThread) {
        std::printf("MISMATCH scope_%dthreads: %ld scopes, expected %ld\n", threads, total,
                    threads * perThread);
        ++failures;
    }
    std::printf("scope_%dthreads      : %ld scopes  %.1f ns/scope/thread  cpu/wall %.2f  sys %.2f s\n",
                threads, total, s * 1e9 / perThread, (cpuSeconds() - cpu0) / s,
                sysSeconds() - sys0);
    gMt = nullptr;
}

void toStd(ProtoContext* ctx, const char* label, const ProtoString* str,
           const std::string& expected, long n) {
    uint64_t h = 14695981039346656037ULL;
    size_t bytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < n; ++i) {
        std::string s = str->toStdString(ctx);
        bytes += s.size();
        if (i == 0) h = fnv(s, h);
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::string once = str->toStdString(ctx);
    const bool ok = once == expected && bytes == expected.size() * static_cast<size_t>(n);
    if (!ok) ++failures;
    std::printf("toStdString %-7s: %ld calls x %zu B  %.1f ns/call  %.2f ns/byte  fnv %016llx %s\n",
                label, n, expected.size(), sec * 1e9 / n, sec * 1e9 / (double) bytes,
                (unsigned long long) h, ok ? "ok" : "MISMATCH");
}

}  // namespace

int main(int argc, char** argv) {
    const long scale = argc > 1 ? std::atol(argv[1]) : 1;
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    scopeSingle(space, 2000000 * scale);
    scopeThreads(space, 4, 500000 * scale);

    const std::string line =
        "203.0.113.7 - - [25/Oct/2026:10:00:00 +0000] \"GET /api/v1/items HTTP/1.1\" 200 51";
    const std::string intl = "Gr\xc3\xbc\xc3\x9f" "e aus M\xc3\xbcnchen \xe2\x82\xac 42 \xf0\x9f\x98\x80 "
                             "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e ok";
    std::string big;
    while (big.size() < 65536) big += line + "\n";
    const ProtoString* sLine = ProtoString::fromUTF8(ctx, line.c_str());
    const ProtoString* sIntl = ProtoString::fromUTF8(ctx, intl.c_str());
    const ProtoString* sShort = ProtoString::fromUTF8(ctx, "abc");
    const ProtoString* sBig = ProtoString::fromUTF8(ctx, big.c_str());
    toStd(ctx, "line", sLine, line, 1000000 * scale);
    toStd(ctx, "intl", sIntl, intl, 1000000 * scale);
    toStd(ctx, "short", sShort, "abc", 2000000 * scale);
    toStd(ctx, "64KiB", sBig, big, 1000 * scale);

    std::printf("result: %s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
