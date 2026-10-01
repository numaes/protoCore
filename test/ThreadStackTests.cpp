/*
 * ProtoSpace::setThreadStackBytes: the stack of the threads newThread creates.
 *
 * macOS gives a secondary thread 512 KiB and has no process-wide default, so
 * there newThread honours the value itself. Elsewhere the platform's default
 * applies (glibc's pthread_setattr_default_np, Windows' /STACK) and the value
 * is only stored.
 */
#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#if defined(__APPLE__)
#include <pthread.h>
#endif

using namespace proto;

namespace {

std::atomic<std::size_t> g_stackBytes{0};
std::atomic<bool> g_ran{false};

const ProtoObject* measure(ProtoContext*, const ProtoObject*, const ParentLink*,
                           const ProtoList*, const ProtoSparseList*) {
#if defined(__APPLE__)
    g_stackBytes = pthread_get_stacksize_np(pthread_self());
#endif
    g_ran = true;
    return PROTO_NONE;
}

}  // namespace

TEST(ThreadStack, NewThreadHonoursTheRequestedStackWhereThePlatformHasNoDefault) {
    const std::size_t before = ProtoSpace::threadStackBytes();
    constexpr std::size_t kWant = 8u << 20;
    ProtoSpace::setThreadStackBytes(kWant);
    EXPECT_EQ(ProtoSpace::threadStackBytes(), kWant);
    {
        ProtoSpace space;
        g_ran = false;
        const ProtoThread* t = space.newThread(space.rootContext,
                                               ProtoString::createSymbol(space.rootContext, "measure"),
                                               measure, nullptr, nullptr);
        {
            ProtoContext::UnmanagedScope out(space.rootContext);
            while (!g_ran) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const_cast<ProtoThread*>(t)->join(space.rootContext);
        }
#if defined(__APPLE__)
        EXPECT_GE(g_stackBytes.load(), kWant);
#endif
    }
    ProtoSpace::setThreadStackBytes(before);
}
