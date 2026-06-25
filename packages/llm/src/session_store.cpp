#include "session_store.h"
#include "log.h"

#include <sqlite3.h>
#include <filesystem>
#include <sstream>
#include <random>
#include <iomanip>

namespace opencode {

SessionStore::SessionStore(const std::string& db_path) : db_path_(db_path) {
    auto parent = std::filesystem::path(db_path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Failed to open database {}: {}", db_path, sqlite3_errmsg(db_));
        return;
    }

    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    init_tables();
    LOG_INFO("SessionStore opened: {}", db_path);
}

SessionStore::~SessionStore() {
    if (db_) sqlite3_close(db_);
}

void SessionStore::init_tables() {
    exec(R"(
        CREATE TABLE IF NOT EXISTS sessions (
            id         TEXT PRIMARY KEY,
            created_at INTEGER NOT NULL DEFAULT (unixepoch()),
            updated_at INTEGER NOT NULL DEFAULT (unixepoch()),
            metadata   TEXT NOT NULL DEFAULT '{}'
        )
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS messages (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id  TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
            role        TEXT NOT NULL,
            content     TEXT NOT NULL DEFAULT '',
            tool_call_id TEXT,
            tool_name   TEXT,
            created_at  INTEGER NOT NULL DEFAULT (unixepoch())
        )
    )");
    exec(R"(
        CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id)
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS context_snapshots (
            session_id  TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
            source_key  TEXT NOT NULL,
            value_json  TEXT NOT NULL,
            rendered    TEXT NOT NULL,
            updated_at  INTEGER NOT NULL DEFAULT (unixepoch()),
            PRIMARY KEY (session_id, source_key)
        )
    )");
}

void SessionStore::exec(const std::string& sql) {
    std::lock_guard lock(mutex_);
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQL error: {}", err ? err : "unknown");
        sqlite3_free(err);
    }
}

int64_t SessionStore::last_insert_id() {
    return sqlite3_last_insert_rowid(db_);
}

// =============================================================================
// 会话
// =============================================================================

std::string SessionStore::create_session(const std::string& metadata) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO sessions (id, metadata) VALUES (?, ?)", -1, &stmt, nullptr);

    // 生成 UUID-like ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15), dis2(8, 11);
    std::ostringstream ss; ss << std::hex << std::setfill('0');
    for (int i = 0; i < 8; i++)  ss << dis(gen); ss << "-";
    for (int i = 0; i < 4; i++)  ss << dis(gen); ss << "-4";
    for (int i = 0; i < 3; i++)  ss << dis(gen); ss << "-";
    ss << dis2(gen);
    for (int i = 0; i < 3; i++)  ss << dis(gen); ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);
    auto id = ss.str();

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, metadata.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    LOG_DEBUG("session created: {}", id);
    return id;
}

std::optional<SessionData> SessionStore::load_session(const std::string& id) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, metadata FROM sessions WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);

    std::optional<SessionData> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionData s;
        s.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result = std::move(s);
    }
    sqlite3_finalize(stmt);
    return result;
}

void SessionStore::save_session(const std::string& id, const SessionData& data) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE sessions SET updated_at = unixepoch() WHERE id = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SessionStore::delete_session(const std::string& id) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SessionStore::delete_all_sessions() {
    std::lock_guard lock(mutex_);
    char* err = nullptr;
    auto run = [&](const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            LOG_ERROR("SQL error: {}", err ? err : "unknown");
            sqlite3_free(err); err = nullptr;
        }
    };
    run("DELETE FROM context_snapshots");
    run("DELETE FROM messages");
    run("DELETE FROM sessions");
}

std::vector<std::string> SessionStore::list_sessions() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id FROM sessions ORDER BY updated_at DESC LIMIT 100",
        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return result;
}

// =============================================================================
// 消息
// =============================================================================

void SessionStore::append_message(const std::string& session_id, const Message& msg) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO messages (session_id, role, content, tool_call_id, tool_name) "
        "VALUES (?, ?, ?, ?, ?)",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, msg.role.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, msg.content.c_str(), -1, SQLITE_STATIC);
    if (msg.tool_call_id) sqlite3_bind_text(stmt, 4, msg.tool_call_id->c_str(), -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 4);
    if (msg.tool_name) sqlite3_bind_text(stmt, 5, msg.tool_name->c_str(), -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 5);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // 更新会话时间
    sqlite3_prepare_v2(db_,
        "UPDATE sessions SET updated_at = unixepoch() WHERE id = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<Message> SessionStore::load_messages(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    std::vector<Message> result;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT role, content, tool_call_id, tool_name "
        "FROM messages WHERE session_id = ? ORDER BY id",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Message m;
        m.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        m.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (sqlite3_column_type(stmt, 2) != SQLITE_NULL)
            m.tool_call_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL)
            m.tool_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result.push_back(m);
    }
    sqlite3_finalize(stmt);
    return result;
}

// =============================================================================
// Context 快照
// =============================================================================

void SessionStore::save_context_snapshot(const std::string& session_id,
                                          const std::string& key,
                                          const nlohmann::json& value,
                                          const std::string& rendered) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO context_snapshots (session_id, source_key, value_json, rendered, updated_at) "
        "VALUES (?, ?, ?, ?, unixepoch())",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_STATIC);
    auto value_str = value.dump();
    sqlite3_bind_text(stmt, 3, value_str.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, rendered.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<nlohmann::json> SessionStore::load_context_snapshot(
    const std::string& session_id, const std::string& key) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT value_json FROM context_snapshots WHERE session_id = ? AND source_key = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_STATIC);

    std::optional<nlohmann::json> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return result;
}

// =============================================================================
// 增强会话
// =============================================================================

std::vector<SessionInfo> SessionStore::list_sessions_info() {
    std::lock_guard lock(mutex_);
    std::vector<SessionInfo> result;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT s.id, s.metadata, s.created_at, s.updated_at, "
        "(SELECT COUNT(*) FROM messages m WHERE m.session_id = s.id) "
        "FROM sessions s ORDER BY s.updated_at DESC LIMIT 100",
        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionInfo info;
        info.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        info.created_at = sqlite3_column_int64(stmt, 2);
        info.updated_at = sqlite3_column_int64(stmt, 3);
        info.message_count = sqlite3_column_int(stmt, 4);
        auto meta = nlohmann::json::parse(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        info.title = meta.value("title", "Untitled");
        result.push_back(info);
    }
    sqlite3_finalize(stmt);
    return result;
}

void SessionStore::set_title(const std::string& session_id, const std::string& title) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE sessions SET metadata = json_set(metadata, '$.title', ?) WHERE id = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, session_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string SessionStore::get_last_session() {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id FROM sessions ORDER BY updated_at DESC LIMIT 1",
        -1, &stmt, nullptr);
    std::string id;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return id;
}

void SessionStore::create_session_with_id(const std::string& id) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR IGNORE INTO sessions (id) VALUES (?)", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int SessionStore::message_count(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT COUNT(*) FROM messages WHERE session_id = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_STATIC);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

std::vector<std::string> SessionStore::search_sessions(const std::string& query, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT DISTINCT m.session_id FROM messages m "
        "WHERE m.content LIKE ? ORDER BY m.session_id DESC LIMIT ?",
        -1, &stmt, nullptr);
    auto like = "%" + query + "%";
    sqlite3_bind_text(stmt, 1, like.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return result;
}

} // namespace opencode
