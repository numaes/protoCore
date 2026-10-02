/*
 * ProtoSpace::setThreadStackBytes: the stack of the threads newThread creates.
 *
 * Since 2.9.0 the value is honoured on every platform (macOS only in 2.8.0):
 * newThread runs the thread on a native thread created with that much stack.
 * The test checks it twice, from inside the thread:
 *
 *  1. The size the platform reports for the thread
 *     (ProtoSpace::currentThreadStackBytes: GetCurrentThreadStackLimits on
 *     Windows, pthread_getattr_np on Linux, pthread_get_stacksize_np on macOS)
 *     is at least the request.
 *  2. The stack is really usable: a probe recursion with 64 KiB frames goes
 *     half the requested depth, which is more than any platform's default
 *     thread stack holds.  The probe runs ONLY when step 1 passed, so a
 *     thread with a small stack fails the assertion instead of overflowing
 *     and taking the test runner down.
 *
 * kWant is 32 MiB, above every default it must be distinguished from: 512 KiB
 * on macOS, 1 MiB on Windows, and RLIMIT_STACK (usually 8 MiB) on Linux.
 */
#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace proto;

namespace {

constexpr std::size_t kWant = 32u << 20;
constexpr std::size_t kFrame = 64u << 10;

std::atomic<std::size_t> g_stackBytes{0};
std::atomic<bool> g_probed{false};
std::atomic<bool> g_ran{false};

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::size_t probe(std::size_t depth) {
    volatile char frame[kFrame];
    for (std::size_t i = 0; i < kFrame; i += 4096) frame[i] = static_cast<char>(depth);
    if (depth == 0) return static_cast<unsigned char>(frame[0]);
    // Using the frame after the call keeps the recursion from becoming a loop.
    return probe(depth - 1) + static_cast<unsigned char>(frame[4096]);
}

const ProtoObject* measure(ProtoContext*, const ProtoObject*, const ParentLink*,
                           const ProtoList*, const ProtoSparseList*) {
    const std::size_t bytes = ProtoSpace::currentThreadStackBytes();
    g_stackBytes = bytes;
    if (bytes >= kWant) {
        // Half the request: comfortably inside the stack, far beyond 8 MiB.
        probe((kWant / 2) / kFrame);
        g_probed = true;
    }
    g_ran = true;
    return PROTO_NONE;
}

}  // namespace

TEST(ThreadStack, NewThreadHonoursTheRequestedStackSize) {
    const std::size_t before = ProtoSpace::threadStackBytes();
    ProtoSpace::setThreadStackBytes(kWant);
    EXPECT_EQ(ProtoSpace::threadStackBytes(), kWant);
    {
        ProtoSpace space;
        g_ran = false;
        g_probed = false;
        g_stackBytes = 0;
        const ProtoThread* t = space.newThread(space.rootContext,
                                               ProtoString::createSymbol(space.rootContext, "measure"),
                                               measure, nullptr, nullptr);
        {
            ProtoContext::UnmanagedScope out(space.rootContext);
            while (!g_ran) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const_cast<ProtoThread*>(t)->join(space.rootContext);
        }
        EXPECT_GE(g_stackBytes.load(), kWant);
        EXPECT_TRUE(g_probed.load()) << "the probe recursion did not run: stack too small";
    }
    ProtoSpace::setThreadStackBytes(before);
}

TEST(ThreadStack, CurrentThreadStackBytesIsReportedOnSupportedPlatforms) {
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    // The main thread, and a plain std::thread: both have a stack the
    // platform can describe.
    EXPECT_GT(ProtoSpace::currentThreadStackBytes(), 0u);
    std::size_t other = 0;
    std::thread([&] { other = ProtoSpace::currentThreadStackBytes(); }).join();
    EXPECT_GT(other, 0u);
#else
    GTEST_SKIP() << "no way to ask this platform for a thread's stack size";
#endif
}
