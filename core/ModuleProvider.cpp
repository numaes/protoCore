/*
 * ModuleProvider.cpp - FileSystemProvider and base for module providers.
 */

#include "../headers/protoCore.h"
#include "ModuleCache.h"
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "ModuleProvider.h"

namespace proto {

namespace {

std::string joinPath(const std::string& base, const std::string& logicalPath) {
    if (base.empty()) return logicalPath;
    if (logicalPath.empty()) return base;
    bool baseEndsWithSep = !base.empty() && (base.back() == '/' || base.back() == '\\');
    if (baseEndsWithSep) return base + logicalPath;
    bool pathStartsWithSep = !logicalPath.empty() && (logicalPath[0] == '/' || logicalPath[0] == '\\');
    if (pathStartsWithSep) return base + logicalPath;
    return base + "/" + logicalPath;
}

bool pathExists(const std::string& path) {
#if defined(_WIN32)
    return _access(path.c_str(), 0) == 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

bool isFile(const std::string& path) {
#if defined(_WIN32)
    struct _stat st;
    return _stat(path.c_str(), &st) == 0 && (st.st_mode & _S_IFREG);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

} // anonymous namespace

FileSystemProvider::FileSystemProvider(std::string basePath)
    : basePath_(std::move(basePath)), guid_("proto.filesystem"), alias_("filesystem") {}

const ProtoObject* FileSystemProvider::tryLoad(const std::string& logicalPath, ProtoContext* ctx) {
    std::string resolved = joinPath(basePath_, logicalPath);
    if (!pathExists(resolved)) return PROTO_NONE;
    if (isFile(resolved)) {
        const ProtoString* pathStr = ProtoString::fromUTF8String(ctx, resolved.c_str());
        if (!pathStr) return PROTO_NONE;
        const ProtoObject* pathObj = pathStr->asObject(ctx);
        const ProtoObject* module = ctx->newObject(false);
        if (!module) return PROTO_NONE;
        const ProtoString* keyPath = ProtoString::fromUTF8String(ctx, "path");
        if (!keyPath) return PROTO_NONE;
        module = module->setAttribute(ctx, keyPath, pathObj);
        return module;
    }
    return PROTO_NONE;
}

const std::string& FileSystemProvider::getGUID() const { return guid_; }
const std::string& FileSystemProvider::getAlias() const { return alias_; }

} // namespace proto
