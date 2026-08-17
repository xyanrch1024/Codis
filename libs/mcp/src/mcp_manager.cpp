#include "mcp_manager.h"
#include "mcp_tool.h"

#include "log.h"

#include <algorithm>
#include <mutex>

namespace codis::mcp {

McpManager::McpManager(std::vector<McpServerOptions> servers, ToolRegistry* registry)
    : servers_(std::move(servers)), registry_(registry) {
    clients_.reserve(servers_.size());
}

McpManager::~McpManager() { stop_all(); }

std::string McpManager::pick_reg_name(const std::string& tool, const std::string& server,
                                      const std::vector<std::string>& existing) const {
    if (std::find(existing.begin(), existing.end(), tool) == existing.end()) return tool;
    std::string prefixed = server + "_" + tool;
    if (std::find(existing.begin(), existing.end(), prefixed) == existing.end()) return prefixed;
    return "";  // 双重冲突：跳过
}

void McpManager::connect_one(size_t idx) {
    auto& opts = servers_[idx];
    auto client = std::make_unique<McpClient>(opts);
    client->on_disconnect = [this, idx] { reconnect_later(idx); };
    if (!client->start()) return;

    std::vector<McpToolInfo> tools;
    if (!client->list_tools(&tools)) {
        LOG_WARN("mcp[{}]: tools/list failed, server connected but unusable", opts.name);
    }

    // 注册（允许增量/重复注册幂等：register_tool 同名校覆盖）
    for (auto& info : tools) {
        std::string raw_name = info.name;
        std::string reg_name = pick_reg_name(raw_name, opts.name, registry_->list());
        if (reg_name.empty()) {
            LOG_WARN("mcp[{}]: tool '{}' name conflict (twice), skipped",
                     opts.name, raw_name);
            continue;
        }
        registry_->register_tool(std::make_unique<McpTool>(
            reg_name, client.get(), std::move(info)));
        LOG_INFO("mcp[{}]: registered tool '{}' ({})", opts.name, reg_name,
                 reg_name == raw_name ? "as-is" : "prefixed");
    }
    clients_[idx] = std::move(client);
    {
        std::lock_guard lock(tools_mutex_);
        if (tool_counts_.size() <= idx) tool_counts_.resize(idx + 1, 0);
        tool_counts_[idx] = tools.size();
    }
}

json McpManager::status() const {
    json arr = json::array();
    for (size_t i = 0; i < servers_.size(); i++) {
        bool online = i < clients_.size() && clients_[i] && clients_[i]->running();
        int64_t n = 0;
        {
            std::lock_guard lock(tools_mutex_);
            if (i < tool_counts_.size()) n = (int64_t)tool_counts_[i];
        }
        arr.push_back({
            {"name", servers_[i].name},
            {"transport", servers_[i].transport},
            {"online", online},
            {"tool_count", n}
        });
    }
    return arr;
}

void McpManager::reconnect_later(size_t idx) {
    if (idx >= clients_.size() || !clients_[idx]) return;  // 已被 stop/替换
    // 原 client 已断；释放后延时重连（防热循环）
    clients_[idx].reset();
    std::thread([this, idx] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!stopped_.load() && !clients_[idx]) connect_one(idx);
    }).detach();
    LOG_INFO("mcp[{}]: scheduled reconnect", servers_[idx].name);
}

void McpManager::start_all() {
    stopped_ = false;
    clients_.clear();
    clients_.resize(servers_.size());
    for (size_t i = 0; i < servers_.size(); i++) connect_one(i);
    LOG_INFO("mcp: {} server(s) configured, {} connected",
             servers_.size(),
             std::count_if(clients_.begin(), clients_.end(),
                           [](auto& c) { return c != nullptr; }));
}

void McpManager::stop_all() {
    stopped_ = true;
    std::vector<std::unique_ptr<McpClient>> clients;
    clients.swap(clients_);
    for (auto& c : clients) {
        if (c) {
            c->on_disconnect = nullptr;
            c->stop();
        }
    }
}

} // namespace codis::mcp