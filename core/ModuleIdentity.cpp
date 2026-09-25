/*
 * ModuleIdentity.cpp — provider + path + version (P3 D11, ruled 2026-09-24).
 */

#include "../headers/protoCore.h"

namespace proto {

namespace {
    // ASCII unit separator: see the class doc for why not '/' or ':'.
    constexpr char kSep = '\x1F';
}

ModuleIdentity::ModuleIdentity(std::string providerGUID, std::string logicalPath,
                               std::string version)
    : providerGUID_(std::move(providerGUID)),
      logicalPath_(std::move(logicalPath)),
      version_(std::move(version)) {
    key_.reserve(providerGUID_.size() + logicalPath_.size() + version_.size() + 2);
    key_ += providerGUID_;
    key_ += kSep;
    key_ += logicalPath_;
    key_ += kSep;
    key_ += version_;   // empty for a module that declares no version
}

ModuleIdentity ModuleIdentity::unversioned(std::string providerGUID,
                                           std::string logicalPath) {
    return ModuleIdentity(std::move(providerGUID), std::move(logicalPath),
                          std::string());
}

const std::string& ModuleIdentity::getProviderGUID() const { return providerGUID_; }
const std::string& ModuleIdentity::getLogicalPath()  const { return logicalPath_;  }
const std::string& ModuleIdentity::getVersion()      const { return version_;      }
const std::string& ModuleIdentity::asKey()           const { return key_;          }

bool ModuleIdentity::operator==(const ModuleIdentity& other) const {
    return key_ == other.key_;
}

} // namespace proto
