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