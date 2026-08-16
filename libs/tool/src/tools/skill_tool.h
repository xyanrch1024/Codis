#pragma once

#include "tool.h"

#include <filesystem>
#include <string>
#include <vector>

namespace codis::tools {

// 技能元数据（解析自 <dir>/<skill_id>/SKILL.md 头部 frontmatter）
struct SkillInfo {
    std::string id;                  // 目录名（[a-z0-9-]）
    std::string name;                // frontmatter: name
    std::string description;         // frontmatter: description
    std::filesystem::path body_path; // SKILL.md 绝对路径
};

// 技能加载器：扫描目录、解析 SKILL.md 的 YAML 子集 frontmatter（--- 块）
class SkillLoader {
public:
    explicit SkillLoader(std::vector<std::filesystem::path> dirs);
    void scan();                                 // （重）扫描全部目录，结果按 id 排序
    const std::vector<SkillInfo>& all() const { return skills_; }
    const SkillInfo* find(const std::string& id) const;
    std::string body(const SkillInfo& s) const;  // frontmatter 之后的正文（截断保护）

private:
    std::vector<std::filesystem::path> dirs_;
    std::vector<SkillInfo> skills_;
};

// 技能调用工具：注入指令上下文（无副作用，仅读文件）
class SkillTool : public Tool {
public:
    explicit SkillTool(std::vector<std::filesystem::path> dirs = {});
    ToolSchema schema() const override;
    Permission default_permission() const override { return Permission::Allow; }
    ToolResult execute(const ToolCall& call) override;
    const std::vector<SkillInfo>& available() const { return loader_.all(); }

private:
    SkillLoader loader_;
};

} // namespace codis::tools