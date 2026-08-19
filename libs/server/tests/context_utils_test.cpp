// context_utils 单测：重放过滤（含 system 摘要回归）、压缩切分/拼接、token 估算。

#include "context_utils.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace codis;
using namespace codis::context_utils;

namespace {

Message msg(const std::string& role, const std::string& content = "") {
    Message m;
    m.role = role;
    m.content = content;
    return m;
}

} // namespace

TEST(ContextUtils, IsBlank) {
    EXPECT_TRUE(is_blank(""));
    EXPECT_TRUE(is_blank("   \n\t\r "));
    EXPECT_FALSE(is_blank("x"));
    EXPECT_FALSE(is_blank("  x  "));
}

TEST(ContextUtils, IsReplayable) {
    EXPECT_TRUE(is_replayable(msg("system", "上下文摘要（历史压缩）:\n...")));  // 摘要回归
    EXPECT_TRUE(is_replayable(msg("user", "hi")));
    EXPECT_TRUE(is_replayable(msg("tool", "result")));
    EXPECT_TRUE(is_replayable(msg("assistant", "plain reply")));
    EXPECT_FALSE(is_replayable(msg("assistant", "   ")));       // 空白正文无工具引用
    {
        auto m = msg("assistant", "");                          // 工具中转消息（无正文）
        m.tool_call_id = "call_1";
        EXPECT_TRUE(is_replayable(m));
    }
    EXPECT_FALSE(is_replayable(msg("reasoning", "chain of thought")));  // 思维链不入上下文
    EXPECT_FALSE(is_replayable(msg("unknown", "x")));
}

TEST(ContextUtils, ReplayableMessagesAttachesReasoning) {
    // 一轮带思维链的往返：reasoning → assistant(文本) → assistant(工具中转) → tool
    std::vector<Message> hist;
    hist.push_back(msg("user", "question"));
    hist.push_back(msg("reasoning", "thinking..."));
    auto a1 = msg("assistant", "calling tool now");
    auto a2 = msg("assistant", "");
    a2.tool_call_id = "call_1";
    a2.tool_name = "read";
    a2.tool_arguments = json::object();
    hist.push_back(a1);
    hist.push_back(a2);
    hist.push_back(msg("tool", "result"));
    // 下一轮无思维链（非思考响应）
    hist.push_back(msg("reasoning", ""));
    auto a3 = msg("assistant", "done");
    hist.push_back(a3);
    // 又一轮带思维链
    hist.push_back(msg("reasoning", "thinking2"));
    hist.push_back(msg("assistant", "again"));

    auto out = replayable_messages(hist);
    ASSERT_EQ(out.size(), 6u);  // user + a1 + a2 + tool + a3 + a4（reasoning 被吸收）

    auto check = [&](size_t idx, const std::string& expect) {
        ASSERT_TRUE(out[idx].reasoning_content.has_value());
        EXPECT_EQ(*out[idx].reasoning_content, expect);
    };
    check(1, "thinking...");  // 文本 assistant
    check(2, "thinking...");  // 同轮工具中转 assistant 同样挂账
    EXPECT_FALSE(out[3].reasoning_content.has_value());  // tool 无
    EXPECT_FALSE(out[4].reasoning_content.has_value());  // 非思考轮 assistant 无挂账
    check(5, "thinking2");
}

TEST(ContextUtils, ReplayableMessagesClearsOnUser) {
    std::vector<Message> hist;
    hist.push_back(msg("reasoning", "old thinking"));
    hist.push_back(msg("assistant", "a"));
    hist.push_back(msg("user", "new question"));   // 新一轮：上一轮思维链作废
    hist.push_back(msg("assistant", "b"));

    auto out = replayable_messages(hist);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_TRUE(out[0].reasoning_content.has_value());
    EXPECT_FALSE(out[1].reasoning_content.has_value());  // user
    EXPECT_FALSE(out[2].reasoning_content.has_value());  // 新轮 assistant
}

TEST(ContextUtils, AppendWithReasoningCompactPath) {
    std::vector<Message> prefix;
    prefix.push_back(msg("user", "q"));
    prefix.push_back(msg("reasoning", "cot"));
    prefix.push_back(msg("assistant", "text"));

    std::vector<Message> out;
    std::string pending;
    bool last_was_tool_call = false;
    for (auto& m : prefix) append_with_reasoning(out, m, pending, last_was_tool_call);
    ASSERT_EQ(out.size(), 2u);  // reasoning 不入请求体
    EXPECT_EQ(out[0].role, "user");
    EXPECT_EQ(out[1].role, "assistant");
    EXPECT_TRUE(out[1].reasoning_content.has_value());
    EXPECT_EQ(*out[1].reasoning_content, "cot");
}

TEST(ContextUtils, ReplayableMessagesDropsOrphanTool) {
    // 压缩切分拆散后的损坏历史：user → tool(孤儿) → assistant(tc) → tool
    std::vector<Message> hist;
    hist.push_back(msg("user", "q"));
    auto orphan = msg("tool", "result without call");
    orphan.tool_call_id = "call_x";
    hist.push_back(orphan);
    auto a = msg("assistant", "");
    a.tool_call_id = "call_1";
    hist.push_back(a);
    auto t = msg("tool", "ok");
    t.tool_call_id = "call_1";
    hist.push_back(t);
    // assistant 文本后跟 tool（同样非法）也丢
    hist.push_back(msg("assistant", "plain reply"));
    hist.push_back(msg("tool", "stray"));

    auto out = replayable_messages(hist);
    ASSERT_EQ(out.size(), 4u);  // user + assistant(tc) + tool + assistant文本；两个孤儿 tool 被丢
    EXPECT_EQ(out[0].role, "user");
    EXPECT_EQ(out[1].role, "assistant");
    EXPECT_EQ(out[2].role, "tool");
    EXPECT_EQ(out[2].tool_call_id, "call_1");
    EXPECT_EQ(out[3].role, "assistant");
}

TEST(ContextUtils, SanitizeToolMarkupBaifull) {
    std::string s = "<\uFF5Ctool_calls>\n<\uFF5Cinvoke name=\"read\">\n"
                    "<\uFF5Cparameter name=\"filePath\">/a/b.cpp</\uFF5Cparameter>\n"
                    "</\uFF5Cinvoke>\n</\uFF5Ctool_calls>\n剩余正文";
    auto out = sanitize_tool_markup(s);
    EXPECT_EQ(out, "剩余正文");
}

TEST(ContextUtils, SanitizeToolMarkupAcp) {
    std::string s = "上下文摘要（历史压缩）:\n\n<tool_call>read\n"
                    "<arg_key>filePath</arg_key>\n"
                    "<arg_value>/a/b.cpp</arg_value>\n</tool_call>\n然后继续";
    auto out = sanitize_tool_markup(s);
    EXPECT_EQ(out, "上下文摘要（历史压缩）:\n\n然后继续");
}

TEST(ContextUtils, SanitizeToolMarkupKeepsProse) {
    std::string s = "普通文本,包括 <b>标签</b> 与 tool_call 这个词不删\n下一行";
    auto out = sanitize_tool_markup(s);
    EXPECT_EQ(out, s);
}

TEST(ContextUtils, SanitizeToolMarkupStripOnlyBlockInline) {
    std::string s = "前面<\uFF5Ctool_calls>x</\uFF5Ctool_calls>后面";
    auto out = sanitize_tool_markup(s);
    EXPECT_EQ(out, "前面后面");
}

TEST(ContextUtils, SanitizeToolMarkupDropsTuiFrameKeepsProse) {
    // TUI 工具帧显示（┃ 前缀）全删，保留用户真实问题
    std::string s = "┃ <tool_call>read\n"
                    "┃ <arg_key>filePath</arg_key>\n"
                    "┃ <arg_value>/a/b.cpp</arg_value>\n"
                    "┃ </tool_call>\n"
                    "压缩后摘要不太对";
    auto out = sanitize_tool_markup(s);
    EXPECT_EQ(out, "压缩后摘要不太对");
}

TEST(ContextUtils, FormatTranscriptFlattensToolRounds) {
    std::vector<Message> hist;
    hist.push_back(msg("user", "压缩后摘要不太对"));
    auto a = msg("assistant", "");
    a.tool_call_id = "call_1";
    a.tool_name = "read";
    hist.push_back(a);
    hist.push_back(msg("reasoning", "让我查一下"));
    hist.push_back(msg("tool", "1: #include <x>\n2: int main() {}"));
    auto a2 = msg("assistant", "查到了。");
    hist.push_back(a2);

    auto out = format_transcript(hist);
    EXPECT_EQ(out,
              "以下是用户与编码助手之间一段已结束的会话记录：\n\n"
              "[用户] 压缩后摘要不太对\n"
              "[助手] 调用工具: read\n"
              "[工具结果] 1: #include <x>\n2: int main() {}\n"
              "[助手] 查到了。\n\n"
              "记录到此为止，会话已经结束。");
    EXPECT_EQ(out.find("让我查一下"), std::string::npos);  // reasoning 不入转录
}

TEST(ContextUtils, FormatTranscriptStripsMarkup) {
    std::vector<Message> hist;
    auto m = msg("user", "┃ <arg_key>filePath</arg_key>\n压缩后摘要不太对");
    hist.push_back(m);
    auto out = format_transcript(hist);
    EXPECT_EQ(out.find("<arg_key>"), std::string::npos);
    EXPECT_NE(out.find("压缩后摘要不太对"), std::string::npos);
}

TEST(ContextUtils, BuildCompactedHistoryDropsLeadingOrphanTool) {
    std::vector<Message> hist;
    hist.push_back(msg("user", "u1"));
    auto orphan = msg("tool", "orphan");
    orphan.tool_call_id = "call_x";
    hist.push_back(orphan);
    auto a = msg("assistant", "");
    a.tool_call_id = "call_1";
    hist.push_back(a);
    auto t = msg("tool", "ok");
    t.tool_call_id = "call_1";
    hist.push_back(t);

    auto split = split_history(hist, 4);
    ASSERT_FALSE(split.has_value());  // 历史太短走不到压缩
    // 直接构造 split 边界：prefix 含 user+orphan，tail 从 assistant 开始
    CompactSplit s{{hist[0], hist[1]}, {hist[2], hist[3]}};
    auto out = build_compacted_history(s, "summary");
    ASSERT_EQ(out.size(), 4u);  // system + user + assistant(tc) + tool（孤儿已随 prefix 丢弃）
    EXPECT_EQ(out[0].role, "system");
    EXPECT_EQ(out[1].role, "user");
    EXPECT_EQ(out[2].role, "assistant");
    EXPECT_EQ(out[3].role, "tool");

    // tail 开头就是孤儿 tool（其 assistant 在 prefix 里被摘要掉）：直接丢
    CompactSplit s2{{hist[0], hist[1]}, {hist[1], hist[2], hist[3]}};
    auto out2 = build_compacted_history(s2, "summary");
    ASSERT_EQ(out2.size(), 4u);  // 孤儿 tool 被丢，assistant(tc)+tool 保留
    EXPECT_EQ(out2[1].role, "user");
    EXPECT_EQ(out2[2].role, "assistant");
    EXPECT_EQ(out2[3].role, "tool");
}

TEST(ContextUtils, ClampKeep) {
    EXPECT_EQ(clamp_keep(0), kCompactMinKeep);
    EXPECT_EQ(clamp_keep(1), kCompactMinKeep);
    EXPECT_EQ(clamp_keep(4), 4);
    EXPECT_EQ(clamp_keep(20), 20);
    EXPECT_EQ(clamp_keep(999), kCompactMaxKeep);
    EXPECT_EQ(clamp_keep(-5), kCompactMinKeep);
}

TEST(ContextUtils, SplitHistoryBasic) {
    std::vector<Message> hist;
    for (int i = 0; i < 10; i++) hist.push_back(msg("user", "u" + std::to_string(i)));
    auto split = split_history(hist, 4);
    ASSERT_TRUE(split.has_value());
    EXPECT_EQ(split->prefix.size(), 6u);
    EXPECT_EQ(split->tail.size(), 4u);
    EXPECT_EQ(split->prefix[0].content, "u0");
    EXPECT_EQ(split->tail[0].content, "u6");
}

TEST(ContextUtils, SplitHistoryTooShort) {
    std::vector<Message> hist;
    for (int i = 0; i < 5; i++) hist.push_back(msg("user", "u"));
    EXPECT_FALSE(split_history(hist, 4).has_value());   // 5 <= 5
    EXPECT_FALSE(split_history(hist, 3).has_value());   // keep clamp 到 4，仍 5 <= 5
    EXPECT_FALSE(split_history(hist, 2).has_value());   // keep clamp 到 4，仍 5 <= 5
}

TEST(ContextUtils, SplitHistoryAfterClamp) {
    std::vector<Message> hist;
    for (int i = 0; i < 6; i++) hist.push_back(msg("user", "u"));
    ASSERT_TRUE(split_history(hist, 3).has_value());    // clamp 到 4：6 > 5
    EXPECT_EQ(split_history(hist, 2)->prefix.size(), 2u);
    EXPECT_EQ(split_history(hist, 2)->tail.size(), 4u);
}

TEST(ContextUtils, SplitPreservesOrder) {
    std::vector<Message> hist;
    for (int i = 0; i < 10; i++)
        hist.push_back(msg(i % 2 ? "assistant" : "user", "m" + std::to_string(i)));
    auto split = split_history(hist, 4);
    ASSERT_TRUE(split.has_value());
    std::vector<Message> merged = split->prefix;
    merged.insert(merged.end(), split->tail.begin(), split->tail.end());
    ASSERT_EQ(merged.size(), hist.size());
    for (size_t i = 0; i < hist.size(); i++) {
        EXPECT_EQ(merged[i].role, hist[i].role);
        EXPECT_EQ(merged[i].content, hist[i].content);
    }
}

TEST(ContextUtils, BuildCompactedHistory) {
    std::vector<Message> hist;
    for (int i = 0; i < 10; i++)
        hist.push_back(msg(i == 0 ? "user" : "assistant", "c" + std::to_string(i)));
    auto split = split_history(hist, 4);
    ASSERT_TRUE(split.has_value());
    auto out = build_compacted_history(*split, "the summary");
    ASSERT_EQ(out.size(), 1u + 1u + 4u);
    EXPECT_EQ(out[0].role, "system");
    EXPECT_EQ(out[0].content, "上下文摘要（历史压缩）:\nthe summary");
    EXPECT_EQ(out[1].role, "user");       // 题干 = prefix 首条 user
    EXPECT_EQ(out[1].content, "c0");
    EXPECT_EQ(out[2].content, "c6");      // 尾部原文顺序保持
    EXPECT_EQ(out.back().content, "c9");
}

TEST(ContextUtils, BuildCompactedHistoryNoUserInPrefix) {
    std::vector<Message> hist;
    for (int i = 0; i < 10; i++) hist.push_back(msg("assistant", "c" + std::to_string(i)));
    auto split = split_history(hist, 4);
    ASSERT_TRUE(split.has_value());
    auto out = build_compacted_history(*split, "s");
    ASSERT_EQ(out.size(), 1u + 4u);
    EXPECT_EQ(out[0].role, "system");
    EXPECT_EQ(out[1].role, "assistant");
}

TEST(ContextUtils, BuildCompactedHistoryEmptySummary) {
    std::vector<Message> hist;
    for (int i = 0; i < 10; i++) hist.push_back(msg("user", "u"));
    auto split = split_history(hist, 4);
    ASSERT_TRUE(split.has_value());
    auto out = build_compacted_history(*split, "");
    ASSERT_EQ(out.size(), 6u);
    EXPECT_EQ(out[0].content, "上下文摘要（历史压缩）:\n");
}

TEST(ContextUtils, EstTokens) {
    EXPECT_EQ(est_tokens({}), 0);
    EXPECT_EQ(est_tokens({msg("user", "")}), 0);
    EXPECT_EQ(est_tokens({msg("user", "abcd")}), 1);
    EXPECT_EQ(est_tokens({msg("user", "abcd"), msg("assistant", "abcd")}), 2);
}