#pragma once

#include "types.h"
#include "tool.h"
#include "session_store.h"

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace opencode {

// =============================================================================
// Context Value
// =============================================================================

struct ContextValue {
    nlohmann::json raw;         // 原始值 (用于快照对比)
    std::string rendered;       // 模型可见文本
    bool available = true;
};

// =============================================================================
// Context Source
// =============================================================================

struct ContextSource {
    std::string key;             // 稳定标识符
    std::string description;

    // 加载器: 返回可用状态 + 值
    using Loader = std::function<ContextValue()>;
    Loader loader;

    // 渲染器: 将值转为模型可见文本
    using Renderer = std::function<std::string(const ContextValue&)>;
    Renderer render;

    // 可选: 增量更新渲染 (用于 mid-conversation message)
    // 如果为空，使用 render 作为 update
    std::optional<Renderer> render_update;
};

// =============================================================================
// System Context
// =============================================================================

class SystemContext {
public:
    void register_source(ContextSource source);
    void remove_source(const std::string& key);

    // 构建 baseline system message (首次 turn / compaction 后)
    std::string build_baseline(const std::string& session_id, SessionStore& store);

    // 对比快照, 返回增量 mid-conversation system message
    // 返回 nullopt 表示无变化
    std::optional<std::string> reconcile(const std::string& session_id, SessionStore& store);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, ContextSource> sources_;
};

// =============================================================================
// 内置 Context Sources 工厂
// =============================================================================

namespace context_sources {

ContextSource date_source();
ContextSource working_dir_source();
ContextSource platform_source();
ContextSource git_status_source();
ContextSource tools_source(std::function<std::vector<ToolSchema>()> tool_fn);
ContextSource project_instructions_source(const std::string& project_root);

} // namespace context_sources

} // namespace opencode
