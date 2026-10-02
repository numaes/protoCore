// SanitizerSupport.h -- test-side detection of the ThreadSanitizer build.
//
// Defines PROTO_TEST_TSAN when the test binary is compiled with
// -fsanitize=thread (GCC defines __SANITIZE_THREAD__; Clang exposes
// __has_feature(thread_sanitizer)), and declares the two TSan runtime entry
// points the suite uses.  Every use is a narrowly scoped exception with a
// comment at the use site saying why.
#pragma once

#if defined(__SANITIZE_THREAD__)
#define PROTO_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PROTO_TEST_TSAN 1
#endif
#endif

#if defined(PROTO_TEST_TSAN)
// Ignore all memory accesses made by the calling thread until the matching
// end call (declared here: not every toolchain's sanitizer/tsan_interface.h
// declares them).
extern "C" void __tsan_ignore_thread_begin();
extern "C" void __tsan_ignore_thread_end();
#endif
