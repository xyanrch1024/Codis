#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace codis {

// TUI 本地状态持久化：~/.config/codis/ui_state.json（XDG_CONFIG_HOME 优先），
// 统一存放上次选择的 provider 与 /yolo 审批策略，切换时写入、启动时读取，
// 保证跨重启不丢。读写失败一律静默（UI 状态非关键路径，不影响主流程）。
inline std::filesystem::path ui_state_path() {
    std::filesystem::path dir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        dir = std::filesystem::path(xdg) / "codis";
    else if (const char* home = std::getenv("HOME"); home && *home)
        dir = std::filesystem::path(home) / ".config" / "codis";
    else
        dir = ".codis";
    return dir / "ui_state.json";
}

inline nlohmann::json load_ui_state() {
    std::ifstream f(ui_state_path());
    if (!f) return nlohmann::json::object();
    try {
        return nlohmann::json::parse(f);
    } catch (...) {
        return nlohmann::json::object();  // 文件损坏则视为空状态
    }
}

inline void save_ui_state(const nlohmann::json& j) {
    auto path = ui_state_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    f << j.dump(2) << "\n";
}

// 上次切换的 provider：switch_provider 成功时写入，TUI 启动时读取并优先于
// server 的 default_provider 生效，保证用户切过的模型跨重启不丢。
inline std::string load_last_provider() {
    auto j = load_ui_state();
    return j.value("last_provider", "");
}

inline void save_last_provider(const std::string& provider) {
    if (provider.empty()) return;
    auto j = load_ui_state();
    j["last_provider"] = provider;
    save_ui_state(j);
}

// /yolo 审批策略：重启后保持用户上次的选择，避免每次手动重开。
inline bool load_yolo() {
    auto j = load_ui_state();
    return j.value("yolo", false);
}

inline void save_yolo(bool on) {
    auto j = load_ui_state();
    j["yolo"] = on;
    save_ui_state(j);
}

} // namespace codis
