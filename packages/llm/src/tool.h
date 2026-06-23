#pragma once

#include <string>
#include <functional>
#include <nlohmann/json.hpp>

namespace opencode {

using json = nlohmann::json;

// =============================================================================
// 权限
// =============================================================================

enum class Permission { Denied, Ask, Allow };

// =============================================================================
// Tool Schema — 用于序列化给 LLM
// =============================================================================

struct ToolSchema {
    std::string name;
    std::string description;
    json parameters;  // JSON Schema
};

// =============================================================================
// Tool Call / Result
// =============================================================================

struct ToolCall {
    std::string id;
    std::string name;
    json arguments;
};

struct ToolResult {
    std::string id;
    bool success = false;
    std::string content;
};

// =============================================================================
// Tool 基类
// =============================================================================

class Tool {
public:
    virtual ~Tool() = default;
    virtual ToolSchema schema() const = 0;
    virtual Permission default_permission() const = 0;
    virtual ToolResult execute(const ToolCall& call) = 0;
};

} // namespace opencode
