#include "skill_tool.h"

#include "log.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace codis::tools {
namespace {

// =============================================================================
// YAML 子集 frontmatter 解析：---\nkey: value\n...\n---
// 支持 name / description（兼容引号：双引号含 \" 转义、单引号 '' 转义、裸值到行尾）
// =============================================================================

std::string unquote(const std::string& raw) {
    std::string v = raw;
    // 去首尾空白后判断引号
    size_t b = v.find_first_not_of(" \t");
    if (b == std::string::npos) { v.clear(); return v; }
    size_t e = v.find_last_not_of(" \t");
    v = v.substr(b, e - b + 1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); i++) {
            if (v[i] == '\\' && i + 2 < v.size() && (v[i + 1] == '"' || v[i + 1] == '\\')) i++;
            out += v[i];
        }
        return out;
    }
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); i++) {
            if (v[i] == '\'' && i + 1 < v.size() - 1 && v[i + 1] == '\'') i++;
            out += v[i];
        }
        return out;
    }
    // 裸值：剥掉尾部行内注释（# 前有空格的）
    if (auto c = v.find(" #"); c != std::string::npos) v = v.substr(0, c);
    if (auto c = v.find('\t'); c != std::string::npos) v = v.substr(0, c);
    return v;
}

// 解析 frontmatter；返回成功与否，成功时填充 name/description（可为空）
bool parse_frontmatter(const std::string& text, std::string* name, std::string* desc) {
    auto is_delim = [](const std::string& line) {
        return line.size() >= 3 && line.compare(0, 3, "---") == 0 &&
               (line.size() == 3 || line[3] == ' ' || line[3] == '\t');
    };
    size_t i = 0;
    while (i < text.size() && (text[i] == '\n' || text[i] == '\r')) i++;
    size_t le = text.find('\n', i);
    std::string first = text.substr(i, le == std::string::npos ? std::string::npos : le - i);
    if (!is_delim(first)) return false;               // 首行必须是 ---
    i = (le == std::string::npos) ? text.size() : le + 1;

    int fields = 0;
    int guard = 0;
    while (i < text.size() && fields < 32 && guard++ < 100) {
        size_t line_end = text.find('\n', i);
        std::string line = text.substr(i, line_end == std::string::npos ? std::string::npos : line_end - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        i = (line_end == std::string::npos) ? text.size() : line_end + 1;
        if (is_delim(line)) return fields > 0;        // 闭合 ---
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;         // 空行
        if (line[b] == '#') continue;                 // 注释
        size_t colon = line.find(':', b);
        if (colon == std::string::npos) continue;     // 非 key: value，忽略
        std::string key = line.substr(b, colon - b);
        if (auto e = key.find_last_not_of(" \t"); e != std::string::npos) key = key.substr(0, e + 1);
        std::string val = unquote(line.substr(colon + 1));
        if (key == "name") { *name = val; fields++; }
        else if (key == "description") { *desc = val; fields++; }
        // 其余键忽略（宽容）
    }
    return fields > 0;
}

std::string read_file(const std::filesystem::path& p, size_t cap = 64 * 1024) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() > cap) buf.resize(cap);
    return buf;
}

bool valid_skill_id(const std::string& id) {
    if (id.empty()) return false;
    for (size_t i = 0; i < id.size(); i++) {
        char c = id[i];
        if (c >= 'a' && c <= 'z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c == '-' && i > 0 && i + 1 < id.size()) continue;
        return false;
    }
    return true;
}

std::string trim_lines(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

// =============================================================================
// SkillLoader
// =============================================================================

SkillLoader::SkillLoader(std::vector<std::filesystem::path> dirs)
    : dirs_(std::move(dirs)) {
    if (dirs_.empty()) {
        // 默认：工作区 skills/ + 用户级 ~/.codis/skills
        dirs_.emplace_back("skills");
        if (const char* home = std::getenv("HOME"))
            dirs_.emplace_back(std::filesystem::path(home) / ".codis" / "skills");
    }
}

void SkillLoader::scan() {
    skills_.clear();
    for (auto& dir : dirs_) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            std::string id = entry.path().filename().string();
            if (!valid_skill_id(id)) continue;
            std::filesystem::path skill_md = entry.path() / "SKILL.md";
            if (!std::filesystem::is_regular_file(skill_md, ec)) continue;

            std::string text = read_file(skill_md);
            if (text.empty()) continue;
            SkillInfo info;
            info.id = id;
            std::string name, description;
            if (parse_frontmatter(text, &name, &description)) {
                info.name = name.empty() ? id : name;
                info.description = description.empty() ? "No description" : description;
            } else {
                LOG_WARN("skill {}: SKILL.md missing frontmatter (--- name/description ---)", id);
                info.name = id;
                info.description = "No description (missing frontmatter)";
            }
            info.body_path = skill_md;
            skills_.push_back(std::move(info));
        }
    }
    std::sort(skills_.begin(), skills_.end(),
              [](const SkillInfo& a, const SkillInfo& b) { return a.id < b.id; });
    for (auto& s : skills_) LOG_INFO("skill registered: {} ({})", s.id, s.name);
}

const SkillInfo* SkillLoader::find(const std::string& id) const {
    for (auto& s : skills_)
        if (s.id == id) return &s;
    return nullptr;
}

std::string SkillLoader::body(const SkillInfo& s) const {
    std::string text = read_file(s.body_path);
    // 剥离 frontmatter：正文从第二个 --- 行之后开始
    size_t p = 0;
    while (p < text.size() && (text[p] == '\n' || text[p] == '\r')) p++;
    if (text.compare(p, 3, "---") != 0) return trim_lines(text);
    p += 3;
    if (p < text.size() && text[p] == '\r') p++;
    if (p < text.size() && text[p] == '\n') p++;
    size_t end = text.find("\n---", p);
    if (end == std::string::npos) return trim_lines(text);
    size_t after = text.find('\n', end + 1);
    if (after == std::string::npos) return {};
    return trim_lines(text.substr(after + 1));
}

// =============================================================================
// SkillTool
// =============================================================================

SkillTool::SkillTool(std::vector<std::filesystem::path> dirs)
    : loader_(std::move(dirs)) {
    loader_.scan();
}

ToolSchema SkillTool::schema() const {
    ToolSchema s;
    s.name = "skill";
    std::string catalog;
    for (auto& sk : loader_.all()) {
        if (!catalog.empty()) catalog += "\n";
        catalog += "- " + sk.id + ": " + sk.description;
    }
    s.description =
        "Load a skill's instructions into context. A skill is a curated set of "
        "instructions for a specific task; after loading, follow its guidance. "
        "This only injects instructions — it performs no actions. Available skills:\n"
        + catalog;
    s.parameters = {{"type", "object"}, {"properties", {
        {"skill", {{"type", "string"}, {"description", "Skill id to load"}}}
    }}, {"required", json::array({"skill"})}};
    return s;
}

ToolResult SkillTool::execute(const ToolCall& call) {
    std::string id;
    if (call.arguments.is_object()) id = call.arguments.value("skill", "");
    const SkillInfo* s = loader_.find(id);
    if (!s || s->name.empty()) {
        std::string avail;
        for (auto& sk : loader_.all()) avail += "- " + sk.id + ": " + sk.description + "\n";
        return {call.id, false,
                "Unknown skill '" + id + "'. Available skills:\n" +
                (avail.empty() ? "(none — no SKILL.md found in skill dirs)" : avail)};
    }
    std::string body = loader_.body(*s);
    if (body.empty()) {
        return {call.id, false, "Skill '" + id + "' found but SKILL.md body is empty."};
    }
    LOG_INFO("skill loaded: {} ({})", s->id, s->name);
    return {call.id, true,
            "技能 " + s->name + "（" + s->id + "）已加载。请严格遵循以下指令完成后续工作：\n\n" + body};
}

} // namespace codis::tools