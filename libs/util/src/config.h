#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdlib>

namespace codis {

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
};

// WebSearch 工具配置
struct WebSearchConfig {
    std::string backend = "bing";   // bing | serpapi | brave | tavily
    std::string api_key;            // 仅 serpapi/brave/tavily 需要
    std::string api_key_env;        // 环境变量名（如 "SERPAPI_API_KEY"）
    int max_results = 5;
    int timeout_seconds = 15;

    void resolve_api_key() {
        if (!api_key.empty()) return;
        if (!api_key_env.empty()) {
            const char* env = std::getenv(api_key_env.c_str());
            if (env) api_key = env;
        }
    }
};

// 工具权限策略：按工具名列表配置，覆盖工具的默认权限声明
struct PermissionConfig {
    std::vector<std::string> allow;   // 免确认直接执行
    std::vector<std::string> ask;     // 执行前征询用户确认
    std::vector<std::string> deny;    // 永远拒绝
    int confirm_timeout_seconds = 120; // Ask 工具等待用户确认的超时，超时视为拒绝
};

struct AppConfig {
    std::vector<ProviderConfig> providers;
    LLMConfig llm;
    std::string default_provider;
    int timeout_seconds = 60;
    PermissionConfig permissions;
    WebSearchConfig websearch;

    static AppConfig load(const std::filesystem::path& path);
    static AppConfig default_config();

    const ProviderConfig* provider_for(const std::string& name) const {
        for (auto& p : providers) {
            if (p.name == name) return &p;
        }
        return nullptr;
    }
};

} // namespace codis
