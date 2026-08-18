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
    std::string proxy;  // HTTP 代理 "host:port"（可选，如 "127.0.0.1:1080"），空则直连
    std::optional<int> max_context_tokens;  // 模型上下文窗口上限（tokens），未配置用内置默认

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

// 技能配置：SKILL.md 扫描目录（相对路径相对 server 工作目录）
struct SkillConfig {
    std::vector<std::filesystem::path> dirs;
};

// MCP 服务器配置（stdio 子进程或 Streamable HTTP）
struct McpServerConfig {
    std::string name;                    // 唯一标识（工具名冲突时作前缀）
    std::string transport = "stdio";     // stdio | http
    std::string command;                 // stdio: 可执行命令
    std::vector<std::string> args;       // stdio: 命令参数
    std::vector<std::string> env;        // stdio: 附加环境变量 "KEY=VALUE"
    std::string url;                     // http: https://host[:port]/path
    std::string api_key_env;             // http: Bearer token 的环境变量名（可选）
    std::string bearer_token;            // http: 解析后的 token（内部）
    int timeout_seconds = 30;
};

struct McpConfig {
    std::vector<McpServerConfig> servers;
};

struct AppConfig {
    std::vector<ProviderConfig> providers;
    LLMConfig llm;
    std::string default_provider;
    int timeout_seconds = 60;
    PermissionConfig permissions;
    WebSearchConfig websearch;
    SkillConfig skills;
    McpConfig mcp;

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
