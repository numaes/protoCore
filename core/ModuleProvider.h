/*
 * ModuleProvider.h - Internal declarations for FileSystemProvider.
 */

#ifndef PROTO_MODULEPROVIDER_H
#define PROTO_MODULEPROVIDER_H

#include "../headers/protoCore.h"
#include <string>

namespace proto {

class FileSystemProvider : public ModuleProvider {
public:
    explicit FileSystemProvider(std::string basePath);

    const ProtoObject* tryLoad(const std::string& logicalPath, ProtoContext* ctx) override;
    const std::string& getGUID() const override;
    const std::string& getAlias() const override;

private:
    std::string basePath_;
    std::string guid_;
    std::string alias_;
};

} // namespace proto

#endif
