#pragma once

#include "tool.h"
#include "log.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <optional>

namespace opencode {

class ToolRegistry {
public:
    void register_tool(std::unique_ptr<Tool> tool) {
        std::unique_lock lock(mutex_);
        auto name = tool->schema().name;
        LOG_INFO("tool '{}' registered", name);
        tools_[name] = std::move(tool);
    }

    std::vector<ToolSchema> all_schemas() const {
        std::shared_lock lock(mutex_);
        std::vector<ToolSchema> schemas;
        for (auto& [_, tool] : tools_) schemas.push_back(tool->schema());
        return schemas;
    }

    Permission check_permission(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = tools_.find(name);
        if (it == tools_.end()) return Permission::Denied;
        return it->second->default_permission();
    }

    ToolResult execute(const ToolCall& call) {
        std::shared_lock lock(mutex_);
        auto it = tools_.find(call.name);
        if (it == tools_.end()) {
            LOG_WARN("unknown tool: {}", call.name);
            return {call.id, false, "Unknown tool: " + call.name};
        }
        LOG_DEBUG("executing tool: {} id={}", call.name, call.id);
        return it->second->execute(call);
    }

    std::vector<std::string> list() const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> names;
        for (auto& [name, _] : tools_) names.push_back(name);
        return names;
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

} // namespace opencode
