#include "tools.h"
#include "log.h"
#include "messages.h"
#include <httplib.h>

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <chrono>
#include <thread>

namespace codis::tools {

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
    if (pipe(out_pipe) == -1) return {call.id, false, "pipe() failed"};
    if (pipe(err_pipe) == -1) {
        // 第二个管道失败时释放第一个，避免 fd 泄漏
        close(out_pipe[0]); close(out_pipe[1]);
        return {call.id, false, "pipe() failed"};
    }

    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：独立进程组，超时可整组击杀（连后代进程一起）
        setpgid(0, 0);
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

    constexpr auto kTimeout = std::chrono::milliseconds(30000);
    auto deadline = std::chrono::steady_clock::now() + kTimeout;
    std::ostringstream out_oss, err_oss;
    char buf[4096];
    int status = 0;
    bool child_done = false;
    bool timed_out = false;
    int open_fds = 2;  // 两个管道读端尚未 EOF

    // 并发读取 stdout/stderr：顺序读会被占着管道不关的后台进程堵死
    while (!child_done || open_fds > 0) {
        if (!child_done) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w > 0) child_done = true;
            else if (w == -1) { child_done = true; status = -1; break; }
        }
        if (open_fds == 0) continue;

        pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        int rc = poll(fds, 2, std::max(0L, remaining.count()));
        if (rc == 0) { timed_out = true; break; }
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].revents & (POLLERR | POLLNVAL)) { open_fds--; continue; }
            if (!(fds[i].revents & (POLLIN | POLLHUP))) continue;
            ssize_t n = read(fds[i].fd, buf, sizeof(buf));
            if (n > 0)
                (i == 0 ? out_oss : err_oss).write(buf, n);
            else  // EOF 或读错误：该管道关闭
                open_fds--;
        }
    }

    if (timed_out) {
        kill(-pid, SIGKILL);  // 击杀整个进程组，避免后台子进程继续存活
        kill(pid, SIGKILL);   // 兜底（setpgid 异常时）
    }
    if (!child_done) waitpid(pid, &status, 0);

    close(out_pipe[0]); close(err_pipe[0]);

    if (timed_out) return {call.id, false, "Command timed out after 30 seconds"};

    std::string output = out_oss.str() + err_oss.str();
    if (output.size() > 64000) {
        output = output.substr(0, 32000) + "\n... [truncated " +
                 std::to_string(output.size() - 64000) + " bytes] ...\n" +
                 output.substr(output.size() - 32000);
    }

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    bool success = WIFEXITED(status) && exit_code == 0;
    // 无输出时给出明确反馈，避免 LLM 无法判断成败而重复执行
    if (output.empty())
        output = "(exit code: " + std::to_string(exit_code) +
                 (success ? ", no output)" : ")");
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

// =============================================================================
// WebSearch — 联网搜索，返回标题 + URL + 摘要
// =============================================================================

namespace {

// URL 百分号编码（query 参数用）
std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// HTML 实体解码（常见子集 + 十进制/十六进制数字实体）
void decode_html_entities(std::string& s) {
    static const std::pair<const char*, const char*> kEnts[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
        {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}, {"&ensp;", " "},
        {"&emsp;", " "}, {"&middot;", "\xC2\xB7"}, {"&ndash;", "-"},
    };
    for (auto& [ent, rep] : kEnts) {
        size_t pos;
        while ((pos = s.find(ent)) != std::string::npos) {
            s.replace(pos, std::strlen(ent), rep);
            // 跳过替换段，避免重叠替换死循环（如 &amp;lt;）
            pos += strlen(rep);
        }
    }
    // &#123; 与 &#xAB; 数字实体 → UTF-8
    std::regex num_ent(R"(&#(?:0*([1-9]\d{1,6})|x0*([0-9a-fA-F]{1,6}));?)");
    std::smatch m;
    std::string out;
    size_t last = 0;
    std::string::const_iterator it = s.cbegin();
    while (std::regex_search(it, s.cend(), m, num_ent)) {
        out.append(it, m[0].first);
        unsigned cp = m[1].matched ? (unsigned)std::stoul(m[1].str())
                                   : (unsigned)std::stoul(m[2].str(), nullptr, 16);
        if (cp <= 0x7F) out += (char)cp;
        else if (cp <= 0x7FF) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        it = m[0].second;
    }
    out.append(it, s.cend());
    s = std::move(out);
}

// 去掉 HTML 标签并折叠空白（供摘要清洗）
std::string strip_html(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool in_tag = false;
    for (char c : src) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        out += c;
    }
    decode_html_entities(out);
    // 折叠空白：连续空白 → 单个空格，去行首尾
    std::string flat;
    flat.reserve(out.size());
    bool prev_space = true;
    for (char c : out) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!prev_space) flat += ' ';
            prev_space = true;
        } else {
            flat += c;
            prev_space = false;
        }
    }
    while (!flat.empty() && flat.back() == ' ') flat.pop_back();
    return flat;
}

// 截断到 max 字符（UTF-8 安全：不切断多字节字符）
std::string utf8_truncate(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && cut < s.size() && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
    s.resize(cut);
    s += "…";
    return s;
}

// 后端 → HTTP 主机
std::string websearch_host(const std::string& backend) {
    if (backend == "bing")   return "https://www.bing.com";
    if (backend == "serpapi") return "https://serpapi.com";
    if (backend == "brave")  return "https://api.search.brave.com";
    if (backend == "tavily") return "https://api.tavily.com";
    return "https://www.bing.com";
}

// Bing RSS → 结果列表（title / url / snippet）
struct SearchHit { std::string title, url, snippet; };

std::vector<SearchHit> parse_bing_rss(const std::string& xml, int max) {
    std::vector<SearchHit> hits;
    size_t pos = 0;
    const std::string kItem = "<item>";
    while (hits.size() < (size_t)max &&
           (pos = xml.find(kItem, pos)) != std::string::npos) {
        size_t end = xml.find("</item>", pos);
        if (end == std::string::npos) break;
        std::string block = xml.substr(pos, end - pos);
        pos = end;

        SearchHit h;
        auto grab = [&](const std::string& tag) -> std::string {
            size_t t0 = block.find("<" + tag + ">");
            if (t0 == std::string::npos) return "";
            size_t t1 = block.find("</" + tag + ">", t0);
            if (t1 == std::string::npos) return "";
            std::string v = block.substr(t0 + tag.size() + 2, t1 - t0 - tag.size() - 2);
            decode_html_entities(v);
            if (tag == "description") v = strip_html(v);
            return v;
        };
        h.title = grab("title");
        h.url = grab("link");
        h.snippet = grab("description");
        if (!h.title.empty() && !h.url.empty()) hits.push_back(std::move(h));
    }
    return hits;
}

// 相关检测：query 的多个词须出现在结果文本中（防单词命中热门垃圾页）。
// CJK 词无空格分词，整串匹配必然落空，按 2-gram 子串匹配；ASCII 整词匹配。
// 纯数字/停用词不计分；全 CJK 或混合查询要求 >=2 个词命中，单实质词查询 >=1。
bool results_relevant(const std::string& corpus, const std::string& query) {
    static const char* kStopwords[] = {
        "http", "https", "www", "com", "org", "the", "and", "for",
        "with", "from", "that", "this", "are", "was", "not", "you",
    };
    auto is_digits = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(),
            [](unsigned char c) { return std::isdigit(c); });
    };
    auto bigram_hit = [&](const std::string& word) {
        // 中文字节 3 字节/字符，需从字符边界切
        std::u32string q32;
        for (size_t i = 0; i < word.size();) {
            uint32_t cp = (unsigned char)word[i];
            int len = 1;
            if ((word[i] & 0xF0) == 0xF0) { cp = ((word[i]&0x07)<<18)|((word[i+1]&0x3F)<<12)|((word[i+2]&0x3F)<<6)|(word[i+3]&0x3F); len = 4; }
            else if ((word[i] & 0xE0) == 0xE0) { cp = ((word[i]&0x0F)<<12)|((word[i+1]&0x3F)<<6)|(word[i+2]&0x3F); len = 3; }
            else if ((word[i] & 0xC0) == 0xC0) { cp = ((word[i]&0x1F)<<6)|(word[i+1]&0x3F); len = 2; }
            q32 += cp; i += len;
        }
        if (q32.size() < 2) q32 += 32;  // 单字符 + 空格兜底
        for (size_t i = 0; i + 1 < q32.size(); i++) {
            std::u32string gram = q32.substr(i, 2);
            std::string enc;
            for (char32_t ch : gram) {
                if (ch < 0x80) enc += (char)ch;
                else if (ch < 0x800) { enc += (char)(0xC0 | (ch >> 6)); enc += (char)(0x80 | (ch & 0x3F)); }
                else { enc += (char)(0xE0 | (ch >> 12)); enc += (char)(0x80 | ((ch >> 6) & 0x3F)); enc += (char)(0x80 | (ch & 0x3F)); }
            }
            if (corpus.find(enc) != std::string::npos) return true;
        }
        return false;
    };

    std::istringstream qss(query);
    std::string q;
    int hits = 0, cjk_words = 0, ascii_words = 0;
    while (qss >> q) {
        if (is_digits(q)) continue;  // 年月日/数字不计分
        bool stop = false;
        for (auto* w : kStopwords) if (q == w) { stop = true; break; }
        if (stop) continue;
        bool has_cjk = false;
        for (unsigned char c : q)
            if (c >= 0xE4 && c <= 0xE9) { has_cjk = true; break; }
        if (has_cjk) {
            cjk_words++;
            if (bigram_hit(q)) hits++;
        } else {
            if (q.size() >= 3) {
                ascii_words++;
                if (corpus.find(q) != std::string::npos) hits++;
            }
        }
    }
    int need = (cjk_words + ascii_words >= 2) ? 2 : 1;
    return hits >= need;
}

} // namespace

ToolSchema WebSearchTool::schema() const {
    ToolSchema s;
    s.name = "websearch";
    s.description =
        "Search the web for current information and return ranked results "
        "(title, URL, snippet). Use for recent events, docs, prices, news. "
        "Backend configured by [websearch] in config.toml.";
    s.parameters = {{"type", "object"}, {"properties", {
        {"query", {{"type", "string"}, {"description", "Search query"}}},
        {"max_results", {{"type", "integer"}, {"description",
            "Number of results (default: configured max, <= 10)"}}}
    }}, {"required", json::array({"query"})}};
    return s;
}

ToolResult WebSearchTool::execute(const ToolCall& call) {
    std::string query = call.arguments.value("query", "");
    if (query.empty()) return {call.id, false, "query is empty"};
    int max = call.arguments.value("max_results", opts_.max_results);
    max = std::clamp(max, 1, 10);

    LOG_INFO("websearch[{}] query: {}", opts_.backend, query);

    const std::string backend = opts_.backend.empty() ? "bing" : opts_.backend;
    std::string body;
    bool ok = false;

    httplib::Client client(websearch_host(backend));
    client.set_follow_location(true);
    client.set_connection_timeout(opts_.timeout_seconds, 0);
    client.set_read_timeout(opts_.timeout_seconds, 0);

    if (!opts_.proxy.empty()) {
        auto colon = opts_.proxy.find(':');
        if (colon != std::string::npos) {
            client.set_proxy(opts_.proxy.substr(0, colon), std::stoi(opts_.proxy.substr(colon + 1)));
            LOG_DEBUG("websearch using http proxy {}", opts_.proxy);
        } else {
            LOG_WARN("invalid proxy '{}' (expect host:port), ignored", opts_.proxy);
        }
    }

    if (backend == "bing") {
        httplib::Headers headers = {
            {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                           "(KHTML, like Gecko) Chrome/120.0 Safari/537.36"}};
        auto res = client.Get((std::string("/search?format=rss&q=") + url_encode(query) +
                               "&mkt=zh-CN").c_str(),
                              headers);
        if (res && res->status == 200) { body = res->body; ok = true; }
        else LOG_WARN("websearch bing failed: {}", res ? std::to_string(res->status) : "no response");
    } else if (backend == "serpapi") {
        auto res = client.Get(("/search.json?engine=google&q=" + url_encode(query) +
                               "&num=" + std::to_string(max) +
                               "&api_key=" + url_encode(opts_.api_key)).c_str());
        if (res && res->status == 200) { body = res->body; ok = true; }
        else LOG_WARN("websearch serpapi failed: {}", res ? std::to_string(res->status) : "no response");
    } else if (backend == "brave") {
        httplib::Headers headers = {{"X-Subscription-Token", opts_.api_key}};
        auto res = client.Get(("/web/search?q=" + url_encode(query) +
                               "&count=" + std::to_string(max)).c_str(),
                              headers);
        if (res && res->status == 200) { body = res->body; ok = true; }
        else LOG_WARN("websearch brave failed: {}", res ? std::to_string(res->status) : "no response");
    } else if (backend == "tavily") {
        json payload = {{"api_key", opts_.api_key}, {"query", query},
                        {"max_results", max}, {"include_answer", false}};
        auto res = client.Post("/search", json_dump_safe(payload), "application/json");
        if (res && res->status == 200) { body = res->body; ok = true; }
        else LOG_WARN("websearch tavily failed: {}", res ? std::to_string(res->status) : "no response");
    } else {
        return {call.id, false, "unknown websearch backend: " + backend};
    }

    if (!ok) return {call.id, false, "Web search request failed (backend: " + backend +
                                     "). Try again later or check [websearch] config."};

    // ---- 结果解析：统一输出 "1. title\n   url\n   snippet" ----
    std::vector<SearchHit> hits;
    json parsed;
    bool parsed_ok = false;
    try { parsed = json::parse(body); parsed_ok = true; } catch (...) {}

    if (backend == "bing") {
        hits = parse_bing_rss(body, max);
        // 相关性检测：数据中心 IP 常被 Bing 返回无关模板页（RSS 缓存内容）
        if (!hits.empty()) {
            bool relevant = false;
            std::string corpus;
            // 只统计 title+snippet：URL 里的 http/https 子串会污染匹配
            for (auto& h : hits) corpus += h.title + " " + h.snippet + " ";
            relevant = results_relevant(corpus, query);
            if (!relevant) {
                LOG_WARN("websearch bing results look unrelated (datacenter IP?) for query: {}", query);
                return {call.id, false,
                    "Web search unavailable from this network (Bing returned unrelated cached "
                    "content). Retry later, or configure a key-based backend ([websearch] "
                    "backend = serpapi|brave|tavily with api_key)."};
            }
        }
    } else if (backend == "serpapi" && parsed_ok) {
        if (parsed.contains("organic_results") && parsed["organic_results"].is_array()) {
            for (auto& r : parsed["organic_results"]) {
                if ((int)hits.size() >= max) break;
                SearchHit h;
                h.title = r.value("title", "");
                h.url = r.value("link", "");
                h.snippet = r.value("snippet", "");
                if (!h.title.empty() || !h.snippet.empty()) hits.push_back(std::move(h));
            }
        }
    } else if (backend == "brave" && parsed_ok) {
        if (parsed.contains("web") && parsed["web"].contains("results")) {
            for (auto& r : parsed["web"]["results"]) {
                if ((int)hits.size() >= max) break;
                SearchHit h;
                h.title = r.value("title", "");
                h.url = r.value("url", "");
                h.snippet = r.value("description", "");
                if (!h.title.empty() || !h.snippet.empty()) hits.push_back(std::move(h));
            }
        }
    } else if (backend == "tavily" && parsed_ok) {
        if (parsed.contains("results") && parsed["results"].is_array()) {
            for (auto& r : parsed["results"]) {
                if ((int)hits.size() >= max) break;
                SearchHit h;
                h.title = r.value("title", "");
                h.url = r.value("url", "");
                h.snippet = r.value("content", "");
                if (!h.title.empty() || !h.snippet.empty()) hits.push_back(std::move(h));
            }
        }
    }

    if (hits.empty())
        return {call.id, true, "No results found for: " + query};

    // 统一输出格式，控制总长度（每结果最多 ~700 字符）
    std::string out;
    out.reserve(4096);
    for (int i = 0; i < (int)hits.size(); i++) {
        auto& h = hits[i];
        out += std::to_string(i + 1) + ". " + utf8_truncate(h.title, 160) + "\n";
        out += "   " + utf8_truncate(h.url, 200) + "\n";
        if (!h.snippet.empty())
            out += "   " + utf8_truncate(h.snippet, 300) + "\n";
        out += "\n";
    }
    return {call.id, true, out};
}

} // namespace codis::tools
