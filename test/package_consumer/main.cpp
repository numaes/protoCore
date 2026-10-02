// See CMakeLists.txt: a smoke test of an installed protoCore package.
#include <protoCore.h>

#include <cstdio>
#include <string>

int main() {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;
    // 2^64 * 3 / 7 through the bignum path, checked against a known value.
    const proto::ProtoObject* big = ctx->fromLong(1LL << 32)->multiply(ctx, ctx->fromLong(1LL << 32));
    const proto::ProtoObject* q = big->multiply(ctx, ctx->fromLong(3))->divide(ctx, ctx->fromLong(7));
    std::string text;
    q->asIntegerString(ctx, 10)->toUTF8String(ctx, text);
    const bool ok = text == "7905747460161236406";
    std::printf("protoCore package consumer: 3*2^64/7 = %s (%s), stack %zu bytes\n",
                text.c_str(), ok ? "ok" : "WRONG", proto::ProtoSpace::currentThreadStackBytes());
    return ok ? 0 : 1;
}
