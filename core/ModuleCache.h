/*
 * ModuleCache.h - Internal API for SharedModuleCache (used by getImportModule).
 */

#ifndef PROTO_MODULECACHE_H
#define PROTO_MODULECACHE_H

#include "../headers/protoCore.h"
#include <string>

namespace proto {

// P3 D11: the cache is keyed by ModuleIdentity::asKey() — provider GUID +
// logical path + version — not by the bare path.  Path alone aliased silently:
// `provider:st/counter_lib` and a local `counter_lib` collapsed into one module
// with the first load winning for both.
const ProtoObject* sharedModuleCacheGet(const ModuleIdentity& id);
void sharedModuleCacheInsert(const ModuleIdentity& id, const ProtoObject* module);

} // namespace proto

#endif
