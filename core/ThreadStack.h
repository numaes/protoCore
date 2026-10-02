/*
 * ThreadStack.h - native threads with a chosen stack size.
 *
 * Internal to protoCore.  Kept apart from proto_internal.h so that the
 * platform headers it needs (<windows.h>, <pthread.h>) are included by one
 * translation unit only and never reach the rest of the kernel.
 */

#ifndef PROTOCORE_THREAD_STACK_H
#define PROTOCORE_THREAD_STACK_H

#include <cstddef>

namespace proto::threadstack {

    /**
     * Run fn(arg) on a new native thread whose stack is at least `bytes`
     * (rounded up to what the platform accepts), and wait for it to finish.
     *
     * Returns false, without calling fn, when the platform refuses to create
     * such a thread (for example a size beyond what it can reserve).  The
     * caller decides what to do then; it must not assume fn ran.
     */
    bool runOnSizedThread(std::size_t bytes, void (*fn)(void*), void* arg);

    /**
     * The stack size of the calling thread as the platform reports it: the
     * reservation on Windows (GetCurrentThreadStackLimits), the pthread stack
     * size on Linux (pthread_getattr_np) and macOS (pthread_get_stacksize_np).
     * 0 where the platform offers no way to ask.
     */
    std::size_t currentThreadStackBytes();

} // namespace proto::threadstack

#endif // PROTOCORE_THREAD_STACK_H
