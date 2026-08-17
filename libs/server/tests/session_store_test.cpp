// SessionStore（SQLite 持久化）单测：会话/消息/压缩重写/快照/级联删除/脏数据容错。

#include "session_store.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>

namespace {

using codis::Message;
using codis::SessionStore;

Message user_msg(const std::string& content) {
    Message m;
    m.role = "user";
    m.content = content;
    return m;
}

Message asst_tool_msg(const std::string& id, const std::string& name) {
    Message m;
    m.role = "assistant";
    m.tool_call_id = id;
    m.tool_name = name;
    m.tool_arguments = nlohmann::json{{"command", "ls"}};
    return m;
}

Message tool_result_msg(const std::string& id, const std::string& content) {
    Message m;
    m.role = "tool";
    m.content = content;
    m.tool_call_id = id;
    return m;
}

// 每个用例独立 :memory: DB，互不污染
class SessionStoreTest : public ::testing::Test {
protected:
    SessionStore store{":memory:"};
};

} // namespace

TEST_F(SessionStoreTest, LoadMissingSessionReturnsNullopt) {
    EXPECT_FALSE(store.load_session("nope").has_value());
}

TEST_F(SessionStoreTest, CreateAndLoadSession) {
    auto id = store.create_session();
    EXPECT_FALSE(id.empty());
    ASSERT_TRUE(store.load_session(id).has_value());
    EXPECT_EQ(store.load_session(id)->id, id);
}

TEST_F(SessionStoreTest, CreateWithIdAndList) {
    store.create_session_with_id("s1");
    store.create_session_with_id("s2");
    EXPECT_TRUE(store.load_session("s1").has_value());
    EXPECT_TRUE(store.load_session("s2").has_value());
    EXPECT_EQ(store.list_sessions_info().size(), 2u);
    auto last = store.get_last_session();
    EXPECT_TRUE(last == "s1" || last == "s2");
}

TEST_F(SessionStoreTest, MessageRoundtrip) {
    store.create_session_with_id("s1");
    store.append_message("s1", user_msg("hello"));
    store.append_message("s1", {"assistant", "hi"});
    auto msgs = store.load_messages("s1");
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].role, "user");
    EXPECT_EQ(msgs[0].content, "hello");
    EXPECT_EQ(msgs[1].role, "assistant");
    EXPECT_EQ(msgs[1].content, "hi");
    EXPECT_EQ(store.message_count("s1"), 2);
}

TEST_F(SessionStoreTest, ToolCallRoundtrip) {
    store.create_session_with_id("s1");
    store.append_message("s1", asst_tool_msg("call_1", "bash"));
    store.append_message("s1", tool_result_msg("call_1", "ok"));
    auto msgs = store.load_messages("s1");
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].role, "assistant");
    ASSERT_TRUE(msgs[0].tool_call_id.has_value());
    EXPECT_EQ(*msgs[0].tool_call_id, "call_1");
    ASSERT_TRUE(msgs[0].tool_name.has_value());
    EXPECT_EQ(*msgs[0].tool_name, "bash");
    ASSERT_TRUE(msgs[0].tool_arguments.has_value());
    EXPECT_EQ((*msgs[0].tool_arguments)["command"], "ls");
    EXPECT_EQ(msgs[1].role, "tool");
    ASSERT_TRUE(msgs[1].tool_call_id.has_value());
    EXPECT_EQ(*msgs[1].tool_call_id, "call_1");
    EXPECT_EQ(msgs[1].content, "ok");
    EXPECT_FALSE(msgs[1].tool_name.has_value());
}

TEST_F(SessionStoreTest, BlankContentAndNullOptionalsRoundtrip) {
    store.create_session_with_id("s1");
    store.append_message("s1", {"assistant", ""});
    auto msgs = store.load_messages("s1");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_TRUE(msgs[0].content.empty());
    EXPECT_FALSE(msgs[0].tool_call_id.has_value());
    EXPECT_FALSE(msgs[0].tool_arguments.has_value());
}

TEST_F(SessionStoreTest, ReplaceMessagesRewritesHistory) {
    store.create_session_with_id("s1");
    for (int i = 0; i < 5; i++) store.append_message("s1", user_msg("u" + std::to_string(i)));
    std::vector<Message> compacted;
    compacted.push_back({"system", "上下文摘要（历史压缩）:\n...summary"});
    compacted.push_back(user_msg("u0"));
    compacted.push_back(user_msg("u4"));
    store.replace_messages("s1", compacted);
    auto msgs = store.load_messages("s1");
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0].role, "system");
    EXPECT_NE(msgs[0].content.find("summary"), std::string::npos);
    EXPECT_EQ(msgs[1].content, "u0");
    EXPECT_EQ(msgs[2].content, "u4");
    EXPECT_EQ(store.message_count("s1"), 3);
}

TEST_F(SessionStoreTest, DeleteSessionCascadesToMessagesAndSnapshots) {
    store.create_session_with_id("s1");
    store.append_message("s1", user_msg("x"));
    store.save_context_snapshot("s1", "k", nlohmann::json{{"a", 1}}, "rendered");
    EXPECT_EQ(store.load_messages("s1").size(), 1u);
    EXPECT_TRUE(store.load_context_snapshot("s1", "k").has_value());
    store.delete_session("s1");
    EXPECT_FALSE(store.load_session("s1").has_value());
    EXPECT_TRUE(store.load_messages("s1").empty());
    EXPECT_FALSE(store.load_context_snapshot("s1", "k").has_value());
}

TEST_F(SessionStoreTest, DeleteAllSessions) {
    store.create_session_with_id("s1");
    store.append_message("s1", user_msg("x"));
    store.create_session_with_id("s2");
    store.delete_all_sessions();
    EXPECT_TRUE(store.list_sessions_info().empty());
    EXPECT_TRUE(store.load_messages("s1").empty());
}

TEST_F(SessionStoreTest, TitleAndListInfo) {
    store.create_session_with_id("s1");
    store.set_title("s1", "hello title");
    auto infos = store.list_sessions_info();
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(infos[0].title, "hello title");
    EXPECT_EQ(infos[0].message_count, 0);
    store.append_message("s1", user_msg("hi"));
    infos = store.list_sessions_info();
    EXPECT_EQ(infos[0].message_count, 1);
}

TEST_F(SessionStoreTest, ContextSnapshotRoundtripAndOverwrite) {
    store.create_session_with_id("s1");
    store.save_context_snapshot("s1", "date", nlohmann::json{{"datetime", "2026-08-17"}}, "<d>x</d>");
    auto v = store.load_context_snapshot("s1", "date");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ((*v)["datetime"], "2026-08-17");
    EXPECT_FALSE(store.load_context_snapshot("s1", "missing").has_value());
    store.save_context_snapshot("s1", "date", nlohmann::json{{"datetime", "2026-08-18"}}, "<d>y</d>");
    v = store.load_context_snapshot("s1", "date");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ((*v)["datetime"], "2026-08-18");
}

// 脏数据容错：tool_arguments 是垃圾字节时解析字段被清空（LOG_WARN），正常行不受影响
TEST(SessionStoreCorruptData, GarbageToolArgumentsTolerated) {
    auto db_path = (std::filesystem::temp_directory_path() /
                    "codis_session_store_test.db").string();
    std::filesystem::remove(db_path);
    {
        SessionStore store(db_path);
        store.create_session_with_id("s1");
        store.append_message("s1", user_msg("good"));
    }
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(db_path.c_str(), &db), SQLITE_OK);
        const char* sql =
            "INSERT INTO messages (session_id, role, content, tool_arguments) "
            "VALUES ('s1', 'assistant', 'bad', ?)";
        sqlite3_stmt* stmt = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
        std::string garbage("\xFF\xFE\x00garbage\xC3", 11);
        sqlite3_bind_text(stmt, 1, garbage.data(), (int)garbage.size(), SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
    {
        SessionStore store(db_path);
        auto msgs = store.load_messages("s1");
        ASSERT_EQ(msgs.size(), 2u);
        EXPECT_EQ(msgs[0].content, "good");
        EXPECT_EQ(msgs[1].content, "bad");
        EXPECT_EQ(msgs[1].role, "assistant");
        EXPECT_FALSE(msgs[1].tool_arguments.has_value());
    }
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}