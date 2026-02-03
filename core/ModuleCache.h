/*
 * ModuleCache.h - Internal API for SharedModuleCache (used by getImportModule).
 */

#ifndef PROTO_MODULECACHE_H
#define PROTO_MODULECACHE_H

#include "../headers/protoCore.h"
#include <string>

namespace proto {

const ProtoObject* sharedModuleCacheGet(const std::string& logicalPath);
void sharedModuleCacheInsert(const std::string& logicalPath, const ProtoObject* module);

} // namespace proto

#endif
