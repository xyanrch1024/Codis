#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdlib>

namespace opencode {

struct ProviderConfig {
    std::string name;
    std::string api_key;
    std::string api_key_env;   // 环境变量名 (如 "DEEPSEEK_API_KEY")
    std::string model;
    std::string base_url;

    // 从环境变量解析 api_key
    void resolve_api_key() {
        if (!api_key.empty()) return;
        if (!api_key_env.empty()) {
            const char* env = std::getenv(api_key_env.c_str());
            if (env) api_key = env;
        }
        // fallback: 尝试 OPENAI_API_KEY
        if (api_key.empty()) {
            const char* env = std::getenv("OPENAI_API_KEY");
            if (env) api_key = env;
        }
    }
};

struct LLMConfig {
    std::optional<int> max_tokens;
    std::optional<double> temperature;
};

struct AppConfig {
    std::vector<ProviderConfig> providers;
    LLMConfig llm;
    std::string default_provider;
    int timeout_seconds = 60;

    static AppConfig load(const std::filesystem::path& path);
    static AppConfig default_config();

    const ProviderConfig* provider_for(const std::string& name) const {
        for (auto& p : providers) {
            if (p.name == name) return &p;
        }
        return nullptr;
    }
};

} // namespace opencode
