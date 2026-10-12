// test/int_alloc/main.cpp — integer operations on two SmallIntegers do not
// allocate.
//
// Before 2.14.6, Integer::compare, sign, negate, abs, divide and modulo
// converted every operand, SmallIntegers included, to a sign-and-magnitude
// temporary holding a std::vector, so each call went through operator new
// and operator delete.  protoST's demo 2 spent 18 % of its single-thread
// time in malloc/free on that path, and its speed depended on the glibc
// version (protoST report 2026-10-12-pre-talk-063, section 2).
//
// This program replaces the global operator new to count the allocations
// the calling thread makes while it runs N operations of each kind, and
// fails if any operation allocated.  A separate executable, because the
// replacement applies to the whole process.  Not built on Windows: a DLL
// resolves operator new inside its own C runtime, so the replacement would
// not see protoCore's allocations there.

#include "../../headers/protoCore.h"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
thread_local bool counting = false;
thread_local unsigned long long allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
    if (counting) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    if (counting) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (counting) ++allocations;
    return std::malloc(size ? size : 1);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (counting) ++allocations;
    return std::malloc(size ? size : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

using namespace proto;

namespace {

constexpr int kN = 10000;
int failures = 0;

template <typename F>
void expectNoAllocation(const char* what, F&& op) {
    allocations = 0;
    counting = true;
    long long sink = 0;
    for (int k = 0; k < kN; ++k) sink += op(k);
    counting = false;
    std::printf("%-34s %8d calls  %8llu allocations  (checksum %lld)\n", what, kN, allocations, sink);
    if (allocations != 0) ++failures;
}

}  // namespace

int main() {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoObject* seven = c->fromLong(7);
    const ProtoObject* minusThree = c->fromLong(-3);
    const ProtoObject* zero = c->fromLong(0);

    expectNoAllocation("ProtoObject::compare", [&](int k) {
        return (long long) c->fromLong(k)->compare(c, seven);
    });
    expectNoAllocation("ProtoObject::partialCompare", [&](int k) {
        return (long long) (c->fromLong(k)->partialCompare(c, minusThree) < 0);
    });
    expectNoAllocation("ProtoObject::integerSign", [&](int k) {
        return (long long) c->fromLong(k - kN / 2)->integerSign(c);
    });
    expectNoAllocation("ProtoObject::negate", [&](int k) {
        return c->fromLong(k)->negate(c)->asLong(c);
    });
    expectNoAllocation("ProtoObject::abs", [&](int k) {
        return c->fromLong(-k)->abs(c)->asLong(c);
    });
    expectNoAllocation("ProtoObject::divide", [&](int k) {
        return c->fromLong(k * 13 - 5)->divide(c, minusThree)->asLong(c);
    });
    expectNoAllocation("ProtoObject::modulo", [&](int k) {
        return c->fromLong(k * 13 - 5)->modulo(c, seven)->asLong(c);
    });
    expectNoAllocation("compare with zero (sign test)", [&](int k) {
        return (long long) c->fromLong(k - 3)->compare(c, zero);
    });

    if (failures) {
        std::printf("FAILED: %d operation(s) allocated on SmallIntegers\n", failures);
        return 1;
    }
    std::printf("OK: no allocation on SmallIntegers\n");
    return 0;
}
