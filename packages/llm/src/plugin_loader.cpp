#include "plugin_loader.h"
#include "log.h"

#include <dlfcn.h>
#include <filesystem>
#include <cstring>
#include <atomic>

namespace opencode {

// 全局桥接（单实例 PluginLoader）
static std::atomic<PluginLoader*> g_active_loader{nullptr};

static void bridge_log(int level, const char* msg) {
    auto* self = g_active_loader.load();
    if (self) self->emit_log(level, msg);
}

static void bridge_register_tool(const char* name, const char* desc,
                                  const char* params, codis_tool_execute_fn exec,
                                  void* ctx) {
    auto* self = g_active_loader.load();
    if (self) self->emit_register_tool(name, desc, params, exec, ctx);
}

static void bridge_register_context(const char* key, const char* desc,
                                     codis_context_loader_fn loader, void* ctx) {
    // placeholder
}

PluginLoader::PluginLoader() { g_active_loader = this; }
PluginLoader::~PluginLoader() { g_active_loader = nullptr; }

void PluginLoader::emit_log(int level, const std::string& msg) {
    if (log_cb_) log_cb_(level, msg);
}

void PluginLoader::emit_register_tool(const std::string& name,
                                       const std::string& desc,
                                       const std::string& params,
                                       codis_tool_execute_fn exec, void* ctx) {
    if (tool_cb_) tool_cb_(name, desc, params, exec, ctx);
}

void PluginLoader::unload_all() {
    std::lock_guard lock(mutex_);
    for (auto& p : plugins_) {
        if (p.shutdown_fn) p.shutdown_fn();
        if (p.handle) dlclose(p.handle);
    }
    plugins_.clear();
}

int PluginLoader::load_directory(const std::string& dir_path) {
    if (!std::filesystem::exists(dir_path)) {
        LOG_WARN("plugin directory not found: {}", dir_path);
        return 0;
    }

    int loaded = 0;
    for (auto& entry : std::filesystem::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".so" || ext == ".dylib") {
            if (load_one(entry.path().string(), ""))
                loaded++;
        }
    }
    LOG_INFO("plugin loader: loaded {} plugins from {}", loaded, dir_path);
    return loaded;
}

bool PluginLoader::load_one(const std::string& path, const std::string& config_json) {
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        LOG_ERROR("dlopen {} failed: {}", path, dlerror());
        return false;
    }

    auto init_fn = reinterpret_cast<codis_plugin_init_fn>(
        dlsym(handle, "plugin_init"));
    if (!init_fn) {
        LOG_ERROR("{} missing plugin_init", path);
        dlclose(handle);
        return false;
    }

    auto shutdown_fn = reinterpret_cast<codis_plugin_shutdown_fn>(
        dlsym(handle, "plugin_shutdown"));

    CodisAPI api{};
    api.version = CODIS_PLUGIN_API_VERSION;
    api.log = bridge_log;
    api.register_tool = bridge_register_tool;
    api.register_context = bridge_register_context;

    int rc = init_fn(&api, config_json.c_str());
    if (rc != 0) {
        LOG_ERROR("{} plugin_init returned {}", path, rc);
        if (shutdown_fn) shutdown_fn();
        dlclose(handle);
        return false;
    }

    LoadedPlugin plugin;
    plugin.path = path;
    plugin.name = std::filesystem::path(path).stem().string();
    plugin.handle = handle;
    plugin.shutdown_fn = shutdown_fn;

    std::lock_guard lock(mutex_);
    plugins_.push_back(plugin);

    LOG_INFO("plugin loaded: {}", plugin.name);
    return true;
}

} // namespace opencode
