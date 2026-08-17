// context_utils 单测：重放过滤（含 system 摘要回归）、压缩切分/拼接、token 估算。

#include "context_utils.h"
#include "test_util.h"

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

void run_context_utils_tests() {
    // ---- is_blank ----
    CHECK(is_blank(""));
    CHECK(is_blank("   \n\t\r "));
    CHECK(!is_blank("x"));
    CHECK(!is_blank("  x  "));

    // ---- is_replayable ----
    CHECK(is_replayable(msg("system", "上下文摘要（历史压缩）:\n...")));  // 摘要回归
    CHECK(is_replayable(msg("user", "hi")));
    CHECK(is_replayable(msg("tool", "result")));
    CHECK(is_replayable(msg("assistant", "plain reply")));
    CHECK(!is_replayable(msg("assistant", "   ")));       // 空白正文无工具引用
    {
        auto m = msg("assistant", "");                    // 工具中转消息（无正文）
        m.tool_call_id = "call_1";
        CHECK(is_replayable(m));
    }
    CHECK(!is_replayable(msg("reasoning", "chain of thought")));  // 思维链不入上下文
    CHECK(!is_replayable(msg("unknown", "x")));

    // ---- clamp_keep ----
    CHECK(clamp_keep(0) == kCompactMinKeep);
    CHECK(clamp_keep(1) == kCompactMinKeep);
    CHECK(clamp_keep(4) == 4);
    CHECK(clamp_keep(20) == 20);
    CHECK(clamp_keep(999) == kCompactMaxKeep);
    CHECK(clamp_keep(-5) == kCompactMinKeep);

    // ---- split_history ----
    {
        std::vector<Message> hist;
        for (int i = 0; i < 10; i++) hist.push_back(msg("user", "u" + std::to_string(i)));
        auto split = split_history(hist, 4);
        CHECK(split.has_value());
        CHECK(split->prefix.size() == 6);
        CHECK(split->tail.size() == 4);
        CHECK(split->prefix[0].content == "u0");
        CHECK(split->tail[0].content == "u6");
    }
    // 过短（clamp 后 <= keep+1）→ nullopt
    {
        std::vector<Message> hist;
        for (int i = 0; i < 5; i++) hist.push_back(msg("user", "u"));
        CHECK(!split_history(hist, 4).has_value());   // 5 <= 5
        CHECK(!split_history(hist, 3).has_value());   // keep clamp 到 4，仍 5 <= 5
        CHECK(!split_history(hist, 2).has_value());   // keep clamp 到 4，仍 5 <= 5
    }
    // clamp 后满足条件则正常切分
    {
        std::vector<Message> hist;
        for (int i = 0; i < 6; i++) hist.push_back(msg("user", "u"));
        CHECK(split_history(hist, 3).has_value());    // clamp 到 4：6 > 5
        CHECK(split_history(hist, 2)->prefix.size() == 2);
        CHECK(split_history(hist, 2)->tail.size() == 4);
    }
    // keep clamp 后仍过短
    {
        std::vector<Message> hist;
        for (int i = 0; i < 3; i++) hist.push_back(msg("user", "u"));
        CHECK(!split_history(hist, 1).has_value());   // clamp 到 4 后 3 <= 5
    }
    // 前缀+尾部拼接 == 原历史
    {
        std::vector<Message> hist;
        for (int i = 0; i < 10; i++) hist.push_back(msg(i % 2 ? "assistant" : "user", "m" + std::to_string(i)));
        auto split = split_history(hist, 4);
        std::vector<Message> merged = split->prefix;
        merged.insert(merged.end(), split->tail.begin(), split->tail.end());
        CHECK(merged.size() == hist.size());
        for (size_t i = 0; i < hist.size(); i++) {
            CHECK(merged[i].role == hist[i].role);
            CHECK(merged[i].content == hist[i].content);
        }
    }

    // ---- build_compacted_history ----
    {
        std::vector<Message> hist;
        for (int i = 0; i < 10; i++) hist.push_back(msg(i == 0 ? "user" : "assistant", "c" + std::to_string(i)));
        auto split = split_history(hist, 4);
        CHECK(split.has_value());
        auto out = build_compacted_history(*split, "the summary");
        CHECK(out.size() == 1 + 1 + 4);
        CHECK(out[0].role == "system");
        CHECK(out[0].content == "上下文摘要（历史压缩）:\nthe summary");
        CHECK(out[1].role == "user");       // 题干 = prefix 首条 user
        CHECK(out[1].content == "c0");
        CHECK(out[2].content == "c6");      // 尾部原文顺序保持
        CHECK(out.back().content == "c9");
    }
    // prefix 无 user（全工具往返）→ 无题干，不崩溃
    {
        std::vector<Message> hist;
        for (int i = 0; i < 10; i++) hist.push_back(msg("assistant", "c" + std::to_string(i)));
        auto split = split_history(hist, 4);
        auto out = build_compacted_history(*split, "s");
        CHECK(out.size() == 1 + 4);
        CHECK(out[0].role == "system");
        CHECK(out[1].role == "assistant");
    }
    // 空摘要也能正常拼接
    {
        std::vector<Message> hist;
        for (int i = 0; i < 10; i++) hist.push_back(msg("user", "u"));
        auto split = split_history(hist, 4);
        auto out = build_compacted_history(*split, "");
        CHECK(out.size() == 6);
        CHECK(out[0].content == "上下文摘要（历史压缩）:\n");
    }

    // ---- est_tokens ----
    CHECK(est_tokens({}) == 0);
    CHECK(est_tokens({msg("user", "")}) == 0);
    CHECK(est_tokens({msg("user", "abcd")}) == 1);
    CHECK(est_tokens({msg("user", "abcd"), msg("assistant", "abcd")}) == 2);
}