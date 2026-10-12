// primes_bench.cpp — the integer operations of protoST's demo 2 (trial
// division) through protoCore's public API: compare, multiply, modulo, add
// on SmallIntegers.  Counts the primes in [1, N] and prints the count, which
// the harness checks (VERIFIED <count>).
//
// build: g++ -O2 -std=c++20 -I<protoCore>/headers primes_bench.cpp -L<lib> -lprotoCore
#include "protoCore.h"
#include <cstdio>
#include <cstdlib>

using namespace proto;

static bool isPrime(ProtoContext* c, const ProtoObject* n) {
    const ProtoObject* two = c->fromInteger(2);
    const ProtoObject* zero = c->fromInteger(0);
    if (n->compare(c, two) < 0) return false;
    if (n->compare(c, c->fromInteger(4)) < 0) return true;
    if (n->modulo(c, two)->compare(c, zero) == 0) return false;
    const ProtoObject* i = c->fromInteger(3);
    while (i->multiply(c, i)->compare(c, n) <= 0) {
        if (n->modulo(c, i)->compare(c, zero) == 0) return false;
        i = i->add(c, two);
    }
    return true;
}

int main(int argc, char** argv) {
    const long long N = argc > 1 ? std::atoll(argv[1]) : 48000;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 20;
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    long long count = 0;
    for (int r = 0; r < rounds; ++r) {
        count = 0;
        for (long long k = 1; k <= N; ++k)
            if (isPrime(c, c->fromLong(k))) ++count;
    }
    std::printf("VERIFIED %lld\n", count);
    return 0;
}
