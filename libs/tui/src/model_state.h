#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace codis {

// 上次切换的 provider 持久化：~/.config/codis/last_provider（XDG_CONFIG_HOME 优先）。
// switch_provider 成功时写入，TUI 启动时读取并优先于 server 的 default_provider 生效，
// 保证用户切过的模型跨重启不丢。读写失败一律静默（UI 状态非关键路径，不影响主流程）。
inline std::filesystem::path last_provider_path() {
    std::filesystem::path dir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        dir = std::filesystem::path(xdg) / "codis";
    else if (const char* home = std::getenv("HOME"); home && *home)
        dir = std::filesystem::path(home) / ".config" / "codis";
    else
        dir = ".codis";
    return dir / "last_provider";
}

inline std::string load_last_provider() {
    std::ifstream f(last_provider_path());
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline void save_last_provider(const std::string& provider) {
    if (provider.empty()) return;
    auto path = last_provider_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    f << provider << "\n";
}

// /yolo 模式持久化：~/.config/codis/yolo，内容 "1"/"0"。
// 与 last_provider 同理：重启后保持用户上次的审批策略，避免每次手动重开。
inline std::filesystem::path yolo_state_path() {
    return last_provider_path().parent_path() / "yolo";
}

inline bool load_yolo() {
    std::ifstream f(yolo_state_path());
    if (!f) return false;
    std::string s;
    std::getline(f, s);
    return s.find('1') != std::string::npos;
}

inline void save_yolo(bool on) {
    auto path = yolo_state_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    f << (on ? "1\n" : "0\n");
}

} // namespace codis
