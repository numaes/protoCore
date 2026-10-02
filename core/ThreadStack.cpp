/*
 * ThreadStack.cpp - native threads with a chosen stack size.  See ThreadStack.h.
 *
 * std::thread cannot be given a stack size, so the threads that
 * ProtoSpace::setThreadStackBytes applies to are created here with the native
 * API of each platform:
 *
 *   Windows  _beginthreadex with STACK_SIZE_PARAM_IS_A_RESERVATION, so the
 *            size is the address space reserved for the stack (what /STACK
 *            sets for the main thread) and not memory committed up front.
 *   POSIX    pthread_create with pthread_attr_setstacksize (Linux, macOS and
 *            any other platform with POSIX threads).
 */

#include "ThreadStack.h"

#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <climits>
#else
#include <pthread.h>
#include <climits>
#include <unistd.h>
#endif

namespace proto::threadstack {

    namespace {
        struct Job {
            void (*fn)(void*);
            void* arg;
        };

#if defined(_WIN32)
        unsigned __stdcall trampoline(void* p) {
            auto* job = static_cast<Job*>(p);
            job->fn(job->arg);
            return 0;
        }
#else
        void* trampoline(void* p) {
            auto* job = static_cast<Job*>(p);
            job->fn(job->arg);
            return nullptr;
        }
#endif
    }

    bool runOnSizedThread(std::size_t bytes, void (*fn)(void*), void* arg) {
        Job job{fn, arg};
#if defined(_WIN32)
        // _beginthreadex takes the size as an unsigned int.  The system rounds
        // the reservation up to its allocation granularity.
        if (bytes > UINT_MAX) return false;
        const uintptr_t handle = _beginthreadex(nullptr, static_cast<unsigned>(bytes), trampoline, &job,
                                                STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (handle == 0) return false;
        WaitForSingleObject(reinterpret_cast<HANDLE>(handle), INFINITE);
        CloseHandle(reinterpret_cast<HANDLE>(handle));
        return true;
#else
        // A whole number of pages, and never below the platform minimum.
        long pageSize = sysconf(_SC_PAGESIZE);
        const std::size_t page = pageSize > 0 ? static_cast<std::size_t>(pageSize) : 16384;
        std::size_t size = bytes < static_cast<std::size_t>(PTHREAD_STACK_MIN)
            ? static_cast<std::size_t>(PTHREAD_STACK_MIN) : bytes;
        if (size > SIZE_MAX - page) return false;
        size = (size + page - 1) / page * page;

        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) return false;
        pthread_t thread;
        const bool started = pthread_attr_setstacksize(&attr, size) == 0 &&
                             pthread_create(&thread, &attr, trampoline, &job) == 0;
        pthread_attr_destroy(&attr);
        if (!started) return false;
        pthread_join(thread, nullptr);
        return true;
#endif
    }

    std::size_t currentThreadStackBytes() {
#if defined(_WIN32)
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        return static_cast<std::size_t>(high - low);
#elif defined(__APPLE__)
        return pthread_get_stacksize_np(pthread_self());
#elif defined(__linux__)
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
        std::size_t size = 0;
        if (pthread_attr_getstacksize(&attr, &size) != 0) size = 0;
        pthread_attr_destroy(&attr);
        return size;
#else
        return 0;
#endif
    }

} // namespace proto::threadstack
