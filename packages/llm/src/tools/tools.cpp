#include "tools.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <chrono>
#include <thread>

namespace opencode::tools {

// =============================================================================
// Bash — fork + exec shell command, capture stdout/stderr
// =============================================================================

ToolSchema BashTool::schema() const {
    ToolSchema s;
    s.name = "bash";
    s.description = "Execute a shell command in a subprocess. "
                    "Use for building, testing, running scripts, git operations. "
                    "Command timeout: 30 seconds. Working directory: project root.";
    s.parameters = {{"type", "object"}, {"properties", {
        {"command", {{"type", "string"}, {"description", "The shell command to execute"}}}
    }}, {"required", json::array({"command"})}};
    return s;
}

ToolResult BashTool::execute(const ToolCall& call) {
    std::string cmd = call.arguments.value("command", "");
    if (cmd.empty()) return {call.id, false, "command is empty"};

    LOG_DEBUG("bash executing: {}", cmd);

    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) == -1 || pipe(err_pipe) == -1) {
        return {call.id, false, "pipe() failed"};
    }

    pid_t pid = fork();
    if (pid == 0) {
        // 子进程
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        rlimit rl{30, 30};  // 30 秒 CPU time
        setrlimit(RLIMIT_CPU, &rl);

        execl("/bin/bash", "bash", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    close(out_pipe[1]); close(err_pipe[1]);

    // 等待超时 (30 秒)
    int status;
    int waited = 0;
    while (waited < 30000) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited += 100;
    }
    if (waited >= 30000) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(out_pipe[0]); close(err_pipe[0]);
        return {call.id, false, "Command timed out after 30 seconds"};
    }

    // 读取输出
    std::ostringstream oss;
    char buf[4096];
    ssize_t n;
    while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0) oss.write(buf, n);
    while ((n = read(err_pipe[0], buf, sizeof(buf))) > 0) oss.write(buf, n);

    close(out_pipe[0]); close(err_pipe[0]);

    std::string output = oss.str();
    if (output.size() > 64000) {
        output = output.substr(0, 32000) + "\n... [truncated " +
                 std::to_string(output.size() - 64000) + " bytes] ...\n" +
                 output.substr(output.size() - 32000);
    }

    bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return {call.id, success, output};
}

// =============================================================================
// Read — read file with optional offset/limit
// =============================================================================

ToolSchema ReadTool::schema() const {
    ToolSchema s;
    s.name = "read";
    s.description = "Read a file from the filesystem. Returns contents with line numbers. "
                    "Supports offset and limit for reading large files in chunks.";
    s.parameters = {{"type", "object"}, {"properties", {
        {"filePath", {{"type", "string"}, {"description", "Absolute path to the file"}}},
        {"offset",   {{"type", "integer"}, {"description", "Line number to start from (1-based)"}}},
        {"limit",    {{"type", "integer"}, {"description", "Max lines to read"}}}
    }}, {"required", json::array({"filePath"})}};
    return s;
}

ToolResult ReadTool::execute(const ToolCall& call) {
    std::string path = call.arguments.value("filePath", "");
    int offset = call.arguments.value("offset", 1);
    int limit  = call.arguments.value("limit", 2000);

    if (!std::filesystem::exists(path))
        return {call.id, false, "File not found: " + path};

    std::ifstream file(path);
    if (!file.is_open())
        return {call.id, false, "Cannot open file: " + path};

    std::ostringstream oss;
    std::string line;
    int line_no = 0, written = 0;
    while (std::getline(file, line)) {
        line_no++;
        if (line_no < offset) continue;
        if (written >= limit) break;
        oss << line_no << ": " << line << "\n";
        written++;
    }
    return {call.id, true, oss.str()};
}

// =============================================================================
// Write — create/overwrite file
// =============================================================================

ToolSchema WriteTool::schema() const {
    ToolSchema s;
    s.name = "write";
    s.description = "Create or overwrite a file with the given content. "
                    "Creates parent directories automatically.";
    s.parameters = {{"type", "object"}, {"properties", {
        {"filePath", {{"type", "string"}, {"description", "Absolute path for the new file"}}},
        {"content",  {{"type", "string"}, {"description", "Content to write"}}}
    }}, {"required", json::array({"filePath", "content"})}};
    return s;
}

ToolResult WriteTool::execute(const ToolCall& call) {
    std::string path = call.arguments.value("filePath", "");
    std::string content = call.arguments.value("content", "");

    if (path.empty()) return {call.id, false, "filePath is empty"};

    // 创建父目录
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);

    std::ofstream file(path);
    if (!file.is_open()) return {call.id, false, "Cannot write: " + path};

    file << content;
    file.close();
    return {call.id, true, "File written: " + path + " (" + std::to_string(content.size()) + " bytes)"};
}

// =============================================================================
// Edit — exact string replacement in file
// =============================================================================

ToolSchema EditTool::schema() const {
    ToolSchema s;
    s.name = "edit";
    s.description = "Replace exact string occurrences in a file. "
                    "Performs exact match replacement. Use replaceAll=true for all occurrences.";
    s.parameters = {{"type", "object"}, {"properties", {
        {"filePath",   {{"type", "string"}, {"description", "Absolute path to the file"}}},
        {"oldString",  {{"type", "string"}, {"description", "Exact text to replace"}}},
        {"newString",  {{"type", "string"}, {"description", "Replacement text"}}},
        {"replaceAll", {{"type", "boolean"}, {"description", "Replace all occurrences (default: false)"}}}
    }}, {"required", json::array({"filePath", "oldString", "newString"})}};
    return s;
}

ToolResult EditTool::execute(const ToolCall& call) {
    std::string path = call.arguments.value("filePath", "");
    std::string old_str = call.arguments.value("oldString", "");
    std::string new_str = call.arguments.value("newString", "");
    bool replace_all = call.arguments.value("replaceAll", false);

    if (!std::filesystem::exists(path))
        return {call.id, false, "File not found: " + path};

    std::ifstream in(path);
    if (!in.is_open()) return {call.id, false, "Cannot read: " + path};
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    auto pos = content.find(old_str);
    if (pos == std::string::npos)
        return {call.id, false, "oldString not found in file"};

    int count = 0;
    if (replace_all) {
        size_t p = 0;
        while ((p = content.find(old_str, p)) != std::string::npos) {
            content.replace(p, old_str.size(), new_str);
            p += new_str.size();
            count++;
        }
    } else {
        content.replace(pos, old_str.size(), new_str);
        count = 1;
    }

    // 备份
    std::filesystem::copy_file(path, path + ".bak",
        std::filesystem::copy_options::overwrite_existing);

    std::ofstream out(path);
    if (!out.is_open()) return {call.id, false, "Cannot write: " + path};
    out << content;
    out.close();

    return {call.id, true, "Replaced " + std::to_string(count) + " occurrence(s) in " + path};
}

// =============================================================================
// Glob — file pattern matching
// =============================================================================

ToolSchema GlobTool::schema() const {
    ToolSchema s;
    s.name = "glob";
    s.description = "Find files matching a glob pattern. Returns relative file paths.";
    s.parameters = {{"type", "object"}, {"properties", {
        {"pattern", {{"type", "string"}, {"description", "Glob pattern (e.g. **/*.cpp, src/**/*.h)"}}},
        {"path",    {{"type", "string"}, {"description", "Base directory (default: current directory)"}}}
    }}, {"required", json::array({"pattern"})}};
    return s;
}

ToolResult GlobTool::execute(const ToolCall& call) {
    std::string pattern = call.arguments.value("pattern", "");
    std::string base = call.arguments.value("path", ".");

    if (!std::filesystem::exists(base))
        return {call.id, false, "Directory not found: " + base};

    // 简单 glob: 递归遍历 + 文件名匹配
    std::ostringstream oss;
    int count = 0;

    try {
        for (auto& entry : std::filesystem::recursive_directory_iterator(base)) {
            if (entry.is_regular_file()) {
                auto rel = std::filesystem::relative(entry.path(), base).string();
                // 简单通配符匹配 (仅支持 * 和 **)
                bool match = false;
                if (pattern == "**" || pattern == "*") {
                    match = true;
                } else {
                    // 支持 **/*.cpp 等模式
                    if (pattern.starts_with("**/")) {
                        auto suffix = pattern.substr(3);
                        if (rel.ends_with(suffix) || (suffix == "*" && rel.find('/') != std::string::npos)) {
                            match = true;
                        } else if (rel.find('/') != std::string::npos) {
                            auto filename = std::filesystem::path(rel).filename().string();
                            if (filename.ends_with(suffix)) match = true;
                        }
                    } else if (rel.ends_with(std::filesystem::path(pattern).filename().string())) {
                        match = true;
                    }
                }
                if (match) {
                    oss << rel << "\n";
                    count++;
                    if (count >= 1000) { oss << "... (truncated)\n"; break; }
                }
            }
        }
    } catch (const std::exception& e) {
        return {call.id, false, std::string("glob error: ") + e.what()};
    }

    return {call.id, true, oss.str().empty() ? "No files matched" : oss.str()};
}

// =============================================================================
// Grep — regex content search
// =============================================================================

ToolSchema GrepTool::schema() const {
    ToolSchema s;
    s.name = "grep";
    s.description = "Search file contents using a regular expression pattern. "
                    "Returns matching lines with file paths and line numbers.";
    s.parameters = {{"type", "object"}, {"properties", {
        {"pattern", {{"type", "string"}, {"description", "Regex pattern to search for"}}},
        {"path",    {{"type", "string"}, {"description", "Directory or file to search in"}}},
        {"include", {{"type", "string"}, {"description", "File pattern filter (e.g. *.cpp, *.h)"}}}
    }}, {"required", json::array({"pattern"})}};
    return s;
}

ToolResult GrepTool::execute(const ToolCall& call) {
    std::string pattern_str = call.arguments.value("pattern", "");
    std::string search_path = call.arguments.value("path", ".");
    std::string include_filter = call.arguments.value("include", "");

    if (pattern_str.empty()) return {call.id, false, "pattern is empty"};

    std::regex re;
    try {
        re = std::regex(pattern_str, std::regex::optimize);
    } catch (const std::regex_error& e) {
        return {call.id, false, std::string("Invalid regex: ") + e.what()};
    }

    std::ostringstream oss;
    int total_matches = 0;

    auto search_file = [&](const std::filesystem::path& filepath) {
        if (!include_filter.empty()) {
            auto ext = filepath.extension().string();
            if (!ext.ends_with(include_filter.substr(include_filter.find_last_of('.')))) {
                // 简单扩展名匹配
                std::string expected = include_filter.starts_with("*") ? include_filter.substr(1) : include_filter;
                if (ext != expected) return;
            }
        }
        std::ifstream file(filepath);
        if (!file.is_open()) return;

        std::string line;
        int line_no = 0;
        while (std::getline(file, line)) {
            line_no++;
            if (std::regex_search(line, re)) {
                oss << filepath.string() << ":" << line_no << ": " << line << "\n";
                total_matches++;
                if (total_matches >= 200) {
                    oss << "... (truncated at 200 matches)\n";
                    file.close();
                    return;
                }
            }
        }
    };

    if (std::filesystem::is_regular_file(search_path)) {
        search_file(search_path);
    } else if (std::filesystem::is_directory(search_path)) {
        try {
            for (auto& entry : std::filesystem::recursive_directory_iterator(
                     search_path, std::filesystem::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file() && total_matches < 200) {
                    search_file(entry.path());
                }
            }
        } catch (const std::exception& e) {
            oss << "Search interrupted: " << e.what() << "\n";
        }
    } else {
        return {call.id, false, "Path not found: " + search_path};
    }

    return {call.id, true, oss.str().empty() ? "No matches found" : oss.str()};
}

} // namespace opencode::tools
