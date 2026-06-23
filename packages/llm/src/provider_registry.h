#pragma once

#include "provider.h"
#include "config.h"
#include "openai_compatible_provider.h"

#include <string>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <optional>

namespace opencode {

class ProviderRegistry {
public:
    void register_provider(const ProviderConfig& cfg);
    std::optional<std::shared_ptr<LLMProvider>> get(const std::string& name);
    std::vector<std::string> list() const;
    std::string default_name() const { return default_; }

    void set_default(const std::string& name) { default_ = name; }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<LLMProvider>> providers_;
    std::string default_;
};

inline void ProviderRegistry::register_provider(const ProviderConfig& cfg) {
    std::unique_lock lock(mutex_);
    auto p = std::make_shared<OpenAICompatibleProvider>(cfg);
    providers_[cfg.name] = p;
    if (default_.empty()) default_ = cfg.name;
}

inline std::optional<std::shared_ptr<LLMProvider>> ProviderRegistry::get(const std::string& name) {
    std::shared_lock lock(mutex_);
    auto it = providers_.find(name);
    if (it == providers_.end()) return std::nullopt;
    return it->second;
}

inline std::vector<std::string> ProviderRegistry::list() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> names;
    for (auto& [name, _] : providers_) names.push_back(name);
    return names;
}

} // namespace opencode
