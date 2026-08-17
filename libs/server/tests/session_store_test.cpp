// SessionStore（SQLite 持久化）单测：会话/消息/压缩重写/快照/级联删除/脏数据容错。

#include "session_store.h"
#include "test_util.h"

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

} // namespace

void run_session_store_tests() {
    // :memory: — 每个用例独立 DB，互不污染
    {
        SessionStore store(":memory:");
        CHECK(store.load_session("nope").has_value() == false);

        auto id = store.create_session();
        CHECK(!id.empty());
        CHECK(store.load_session(id).has_value());
        CHECK(store.load_session(id)->id == id);
    }

    {
        SessionStore store(":memory:");
        store.create_session_with_id("s1");
        store.create_session_with_id("s2");
        CHECK(store.load_session("s1").has_value());
        CHECK(store.load_session("s2").has_value());
        CHECK(store.list_sessions_info().size() == 2);
        CHECK(store.get_last_session() == "s1" || store.get_last_session() == "s2");
    }

    // 消息往返：普通 user / assistant 纯文本
    {
        SessionStore store(":memory:");
        store.create_session_with_id("s1");
        store.append_message("s1", user_msg("hello"));
        store.append_message("s1", {"assistant", "hi"});
        auto msgs = store.load_messages("s1");
        CHECK(msgs.size() == 2);
        CHECK(msgs[0].role == "user");
        CHECK(msgs[0].content == "hello");
        CHECK(msgs[1].role == "assistant");
        CHECK(msgs[1].content == "hi");
        CHECK(store.message_count("s1") == 2);
    }

    // 工具往返：tool_call_id / tool_name / tool_arguments(JSON) / tool 结果
    {
        SessionStore store(":memory:");
        store.create_session_with_id("s1");
        store.append_message("s1", asst_tool_msg("call_1", "bash"));
        store.append_message("s1", tool_result_msg("call_1", "ok"));
        auto msgs = store.load_messages("s1");
        CHECK(msgs.size() == 2);
        CHECK(msgs[0].role == "assistant");
        CHECK(msgs[0].tool_call_id.has_value());
        CHECK(*msgs[0].tool_call_id == "call_1");
        CHECK(*msgs[0].tool_name == "bash");
        CHECK(msgs[0].tool_arguments.has_value());
        CHECK((*msgs[0].tool_arguments)["command"] == "ls");
        CHECK(msgs[1].role == "tool");
        CHECK(*msgs[1].tool_call_id == "call_1");
        CHECK(msgs[1].content == "ok");
        CHECK(!msgs[1].tool_name.has_value());
    }

    // 空白 content 与无 optional 字段的往返（load 侧 tool_call_id 为 NULL）
    {
        SessionStore store(":memory:");
        store.create_session_with_id("s1");
        store.append_message("s1", {"assistant", ""});
        auto msgs = store.load_messages("s1");
        CHECK(msgs.size() == 1);
        CHECK(msgs[0].content.empty());
        CHECK(!msgs[0].tool_call_id.has_value());
        CHECK(!msgs[0].tool_arguments.has_value());
    }

    // replace_messages（压缩路径）：单事务 DELETE + 重写，加载顺序一致
    {
        SessionStore store(":memory:");
        store.create_session_with_id("s1");
        for (int i = 0; i < 5; i++) store.append_message("s1", user_msg("u" + std::to_string(i)));
        std::vector<Message> compacted;
        compacted.push_back({"system", "上下文摘要（历史压缩）:\n...summary"});
        compacted.push_back(user_msg("u0"));
        compacted.push_back(user_msg("u4"));
        store.replace_messages("s1", compacted);
        auto msgs = store.load_messages("s1");
        CHECK(msgs.size() == 3);
        CHECK(msgs[0].role == "system");
        CHECK(msgs[0].content.find("summary") != std::string::npos);
        CHECK(msgs[1].content == "u0");
        CHECK(msgs[2].content == "u4");
        CHECK(store.message_count("s1") == 3);
    }

    // FK 级联：删会话连带删消息与快照
    {
        SessionStore store(":memory:");
        store.create_session_with_id("s1");
        store.append_message("s1", user_msg("x"));
        store.save_context_snapshot("s1", "k", nlohmann::json{{"a", 1}}, "rendered");
        CHECK(store.load_messages("s1").size() == 1);
        CHECK(store.load_context_snapshot("s1", "k").has_value());
        store.delete_session("s1");
        CHECK(!store.load_session("s1").has_value());
        CHECK(store.load_messages("s1").empty());
        CHECK(!store.load_context_snapshot("s1", "k").has_value());
    }

    // delete_all_sessions
    {
        SessionStore store(":memory:");
        store.create_session_with_id("s1");
        store.append_message("s1", user_msg("x"));
        store.create_session_with_id("s2");
        store.delete_all_sessions();
        CHECK(store.list_sessions_info().empty());
        CHECK(store.load_messages("s1").empty());
    }

    // 标题与列表信息
    {
        SessionStore store(":memory:");
        store.create_session_with_id("s1");
        store.set_title("s1", "hello title");
        auto infos = store.list_sessions_info();
        CHECK(infos.size() == 1);
        CHECK(infos[0].title == "hello title");
        CHECK(infos[0].message_count == 0);
        store.append_message("s1", user_msg("hi"));
        infos = store.list_sessions_info();
        CHECK(infos[0].message_count == 1);
    }

    // context_snapshot 存取（JSON 往返）
    {
        SessionStore store(":memory:");
        store.create_session_with_id("s1");
        store.save_context_snapshot("s1", "date", nlohmann::json{{"datetime", "2026-08-17"}}, "<d>x</d>");
        auto v = store.load_context_snapshot("s1", "date");
        CHECK(v.has_value());
        CHECK((*v)["datetime"] == "2026-08-17");
        CHECK(!store.load_context_snapshot("s1", "missing").has_value());
        // 同 key 覆盖
        store.save_context_snapshot("s1", "date", nlohmann::json{{"datetime", "2026-08-18"}}, "<d>y</d>");
        v = store.load_context_snapshot("s1", "date");
        CHECK((*v)["datetime"] == "2026-08-18");
    }

    // 脏数据容错：tool_arguments 是垃圾字节时该行被跳过，不拖垮服务
    {
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
            CHECK(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
            const char* sql =
                "INSERT INTO messages (session_id, role, content, tool_arguments) "
                "VALUES ('s1', 'assistant', 'bad', ?)";
            sqlite3_stmt* stmt = nullptr;
            CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
            std::string garbage("\xFF\xFE\x00garbage\xC3", 11);
            sqlite3_bind_text(stmt, 1, garbage.data(), (int)garbage.size(), SQLITE_TRANSIENT);
            CHECK(sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
        }
        {
            SessionStore store(db_path);
            auto msgs = store.load_messages("s1");
            // 脏 tool_arguments 行保留但解析字段被清空（LOG_WARN），正常行不受影响
            CHECK(msgs.size() == 2);
            CHECK(msgs[0].content == "good");
            CHECK(msgs[1].content == "bad");
            CHECK(msgs[1].role == "assistant");
            CHECK(!msgs[1].tool_arguments.has_value());
        }
        std::filesystem::remove(db_path);
        std::filesystem::remove(db_path + "-wal");
        std::filesystem::remove(db_path + "-shm");
    }
}