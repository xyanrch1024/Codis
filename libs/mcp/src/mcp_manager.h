#pragma once

#include "mcp_client.h"
#include "tool_registry.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace codis::mcp {

// MCP 服务器管理器：连接配置的 server、拉取工具列表并批量注册进 ToolRegistry。
// 工具名冲突时自动加 <server>_ 前缀；stdio 进程断开会自动重连并重注册（幂等覆盖）。
class McpManager {
public:
    McpManager(std::vector<McpServerOptions> servers, ToolRegistry* registry);
    ~McpManager();

    McpManager(const McpManager&) = delete;
    McpManager& operator=(const McpManager&) = delete;

    void start_all();
    void stop_all();

    // 各 server 运行状态（供 /api/v1/info 展示）：name/transport/online/工具数
    json status() const;

private:
    void connect_one(size_t idx);
    void reconnect_later(size_t idx);
    std::string pick_reg_name(const std::string& tool, const std::string& server,
                              const std::vector<std::string>& existing) const;

    std::vector<McpServerOptions> servers_;
    ToolRegistry* registry_;
    std::vector<std::unique_ptr<McpClient>> clients_;
    std::atomic<bool> stopped_{false};

    // 每 server 最近一次 tools/list 的工具数（connect_one/重连时更新）
    mutable std::mutex tools_mutex_;
    std::vector<size_t> tool_counts_;
};

} // namespace codis::mcp