// Heap cells reachable from one object built with n successive
// setAttribute calls: the object cell plus its attribute storage cells.
// Compiled twice: against master's internal header (AVL only) and against
// the branch's (-DBRANCH).  Values are SmallIntegers (no cells).
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>
#include "proto_internal.h"
using namespace proto;

static void countAvl(const ProtoSparseListImplementation* n, std::set<const void*>& seen) {
    if (!n || seen.count(n)) return;
    seen.insert(n);
    countAvl(n->previous, seen);
    countAvl(n->next, seen);
}

int main() {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 128; ++i)
        keys.push_back(ProtoString::fromUTF8String(c, ("key" + std::to_string(i)).c_str())->intern(c));
    for (int n : {0, 1, 2, 4, 8, 12, 16, 24, 32, 64, 100}) {
        const ProtoObject* o = c->newObject(false);
        for (int i = 0; i < n; ++i) o = o->setAttribute(c, keys[i], c->fromInteger(i));
        // Same object, then 3n random overwrites (steady state of a live object).
        const ProtoObject* w = o;
        unsigned seed = 12345;
        for (int i = 0; i < 3 * n; ++i) {
            seed = seed * 1103515245u + 12345u;
            w = w->setAttribute(c, keys[(seed >> 8) % n], c->fromInteger(1000 + i));
        }
        size_t built = 0, steady = 0;
        for (int pass = 0; pass < 2; ++pass) {
            const ProtoObjectCell* oc = toImpl<const ProtoObjectCell>(pass ? w : o);
            std::set<const void*> seen;
#ifdef BRANCH
            if (const ProtoAttributeListImplementation* l = oc->attributes.list()) {
                for (; l; l = l->next) seen.insert(l);
            } else if (oc->attributes.avl()) {
                countAvl(oc->attributes.avl(), seen);
            }
#else
            countAvl(oc->attributes, seen);
#endif
            (pass ? steady : built) = 1 + seen.size();
        }
        std::printf("n=%3d live cells per object: built %zu, after 3n overwrites %zu\n", n, built, steady);
    }
    return 0;
}
