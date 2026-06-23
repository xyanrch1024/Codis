#include "context_source.h"
#include "log.h"

#include <chrono>
#include <format>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef __linux__
#include <sys/utsname.h>
#endif

namespace opencode {

// =============================================================================
// SystemContext
// =============================================================================

void SystemContext::register_source(ContextSource source) {
    std::unique_lock lock(mutex_);
    LOG_INFO("context source registered: {}", source.key);
    sources_[source.key] = std::move(source);
}

void SystemContext::remove_source(const std::string& key) {
    std::unique_lock lock(mutex_);
    sources_.erase(key);
}

std::string SystemContext::build_baseline(const std::string& session_id, SessionStore& store) {
    std::shared_lock lock(mutex_);
    std::ostringstream oss;
    oss << "You are an AI coding agent. Use the tools provided to help the user with software engineering tasks.\n\n";

    for (auto& [key, src] : sources_) {
        auto val = src.loader();
        if (!val.available) {
            LOG_WARN("context source '{}' unavailable, skipping", key);
            continue;
        }

        auto rendered = src.render(val);
        oss << rendered << "\n";

        store.save_context_snapshot(session_id, key, val.raw, rendered);
    }

    LOG_DEBUG("baseline built for session {}, {} sources", session_id, sources_.size());
    return oss.str();
}

std::optional<std::string> SystemContext::reconcile(const std::string& session_id, SessionStore& store) {
    std::shared_lock lock(mutex_);
    std::ostringstream oss;
    bool has_changes = false;

    for (auto& [key, src] : sources_) {
        auto val = src.loader();
        if (!val.available) continue;

        auto old_val = store.load_context_snapshot(session_id, key);
        if (old_val && *old_val == val.raw) continue;  // 无变化

        // 有变化 → 生成增量消息
        has_changes = true;
        auto rendered = src.render_update ? (*src.render_update)(val) : src.render(val);
        oss << rendered << "\n";

        store.save_context_snapshot(session_id, key, val.raw, rendered);
        LOG_DEBUG("context source '{}' changed, pushing update", key);
    }

    if (!has_changes) return std::nullopt;
    return oss.str();
}

// =============================================================================
// 内置 Context Sources
// =============================================================================

namespace context_sources {

ContextSource date_source() {
    return {
        .key = "date",
        .description = "Current date and time",
        .loader = []() -> ContextValue {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            std::ostringstream oss;
            oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
            auto str = oss.str();
            return {
                .raw = {{"datetime", str}},
                .rendered = std::format("<current_date>{}</current_date>", str)
            };
        },
        .render = [](const ContextValue& v) { return v.rendered; },
        .render_update = ContextSource::Renderer{
            [](const ContextValue& v) {
                return std::format("The current date is now: {}\n", v.raw["datetime"].get<std::string>());
            }}
    };
}

ContextSource working_dir_source() {
    return {
        .key = "working_dir",
        .description = "Current working directory",
        .loader = []() -> ContextValue {
            auto dir = std::filesystem::current_path().string();
            return {
                .raw = {{"path", dir}},
                .rendered = std::format("<working_directory>{}</working_directory>", dir)
            };
        },
        .render = [](const ContextValue& v) { return v.rendered; }
    };
}

ContextSource platform_source() {
    return {
        .key = "platform",
        .description = "Operating system and environment",
        .loader = []() -> ContextValue {
#ifdef __linux__
            utsname u;
            uname(&u);
            std::string os = std::format("{}/{}", u.sysname, u.release);
#elif defined(_WIN32)
            std::string os = "Windows";
#else
            std::string os = "Unknown";
#endif
            auto shell = std::getenv("SHELL");
            return {
                .raw = {{"os", os}, {"shell", shell ? shell : "unknown"}},
                .rendered = std::format("<platform>OS: {}, Shell: {}</platform>",
                    os, shell ? shell : "unknown")
            };
        },
        .render = [](const ContextValue& v) { return v.rendered; }
    };
}

ContextSource git_status_source() {
    return {
        .key = "git_status",
        .description = "Current Git repository status",
        .loader = []() -> ContextValue {
            std::string output;
            FILE* pipe = popen("git status --porcelain 2>/dev/null", "r");
            if (!pipe) return {.available = false};
            char buf[512];
            while (fgets(buf, sizeof(buf), pipe)) output += buf;
            int rc = pclose(pipe);

            if (rc != 0 || output.empty()) {
                return {.raw = {{"dirty", false}}, .rendered = "<git_status>clean</git_status>"};
            }

            // 截断过长输出
            if (output.size() > 2000) output = output.substr(0, 2000) + "\n...";

            return {
                .raw = {{"dirty", true}, {"porcelain", output}},
                .rendered = std::format("<git_status>\n{}\n</git_status>", output)
            };
        },
        .render = [](const ContextValue& v) { return v.rendered; }
    };
}

ContextSource tools_source(std::function<std::vector<ToolSchema>()> tool_fn) {
    return {
        .key = "tools",
        .description = "Available tools",
        .loader = [tool_fn]() -> ContextValue {
            auto schemas = tool_fn();
            nlohmann::json arr = nlohmann::json::array();
            std::ostringstream oss;
            oss << "<available_tools>\n";
            for (auto& s : schemas) {
                nlohmann::json item;
                item["name"] = s.name;
                item["description"] = s.description;
                arr.push_back(item);
                oss << "- " << s.name << ": " << s.description << "\n";
            }
            oss << "</available_tools>";
            return {.raw = arr, .rendered = oss.str()};
        },
        .render = [](const ContextValue& v) { return v.rendered; }
    };
}

ContextSource project_instructions_source(const std::string& project_root) {
    return {
        .key = "project_instructions",
        .description = "AGENTS.md and CONTEXT.md files",
        .loader = [project_root]() -> ContextValue {
            std::ostringstream oss;
            for (auto& name : {"AGENTS.md", "CONTEXT.md", ".github/AGENTS.md"}) {
                auto path = std::filesystem::path(project_root) / name;
                if (std::filesystem::exists(path)) {
                    std::ifstream f(path);
                    if (f.is_open()) {
                        oss << "<instructions source=\"" << name << "\">\n"
                            << std::string(std::istreambuf_iterator<char>(f),
                                           std::istreambuf_iterator<char>())
                            << "\n</instructions>\n\n";
                    }
                }
            }
            auto content = oss.str();
            return {
                .raw = {{"instructions", content}},
                .rendered = content.empty() ? "" : content
            };
        },
        .render = [](const ContextValue& v) { return v.rendered; }
    };
}

} // namespace context_sources

} // namespace opencode
