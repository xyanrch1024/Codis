#pragma once

#include "plugin.h"

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>

namespace opencode {

using LogCallback = std::function<void(int level, const std::string& msg)>;
using PluginToolRegistrar = std::function<void(const std::string& name,
    const std::string& desc, const std::string& params,
    codis_tool_execute_fn execute, void* ctx)>;

struct LoadedPlugin {
    std::string path;
    std::string name;
    void* handle = nullptr;
    codis_plugin_shutdown_fn shutdown_fn = nullptr;
};

class PluginLoader {
public:
    PluginLoader();
    ~PluginLoader();

    void set_logger(LogCallback cb) { log_cb_ = std::move(cb); }
    void set_tool_registrar(PluginToolRegistrar cb) { tool_cb_ = std::move(cb); }

    void emit_log(int level, const std::string& msg);
    void emit_register_tool(const std::string& name, const std::string& desc,
                            const std::string& params, codis_tool_execute_fn exec,
                            void* ctx);

    int load_directory(const std::string& dir_path);
    void unload_all();
    const std::vector<LoadedPlugin>& plugins() const { return plugins_; }

private:
    bool load_one(const std::string& path, const std::string& config_json);

    std::vector<LoadedPlugin> plugins_;
    std::mutex mutex_;
    LogCallback log_cb_;
    PluginToolRegistrar tool_cb_;
};

} // namespace opencode
