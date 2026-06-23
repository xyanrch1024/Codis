#pragma once

#include "types.h"
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <mutex>

struct sqlite3;

namespace opencode {

struct SessionData {
    std::string id;
    std::string metadata;
};

class SessionStore {
public:
    SessionStore(const std::string& db_path);
    ~SessionStore();

    // 会话
    std::string create_session(const std::string& metadata = "{}");
    std::optional<SessionData> load_session(const std::string& id);
    void save_session(const std::string& id, const SessionData& data);
    void delete_session(const std::string& id);
    std::vector<std::string> list_sessions();

    // 消息
    void append_message(const std::string& session_id, const Message& msg);
    std::vector<Message> load_messages(const std::string& session_id);

    // Context 快照
    void save_context_snapshot(const std::string& session_id,
                                const std::string& key,
                                const nlohmann::json& value,
                                const std::string& rendered);
    std::optional<nlohmann::json> load_context_snapshot(const std::string& session_id,
                                                         const std::string& key);

private:
    void init_tables();
    void exec(const std::string& sql);
    int64_t last_insert_id();

    sqlite3* db_ = nullptr;
    std::mutex mutex_;
    std::string db_path_;
};

} // namespace opencode
