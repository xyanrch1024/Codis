# Phase 7: ReAct + RAG 实现计划

---

## 一、ReAct (Reasoning + Acting)

### 概念

ReAct 是一套 prompt 驱动的循环模式，让 LLM 在"思考"和"执行"之间交替：

```
Thought: 分析了代码结构，需要先读取 server.cpp 了解当前 ACP 实现
Action: read("server.cpp", offset=250, limit=40)
Observation: [文件内容...]

Thought: 看到 run_acp_loop 在第 270 行，需要增加 ReAct 分支逻辑
Action: edit("server.cpp", oldString="...", newString="...")
Observation: Replaced 1 occurrence(s)

Thought: 修改完成，验证编译
Action: bash("cmake --build build")
Observation: Build successful

Final Answer: 已在 server.cpp:270 实现 ReAct 循环
```

### 实现路径

**最小改动方案** — 当前 ACP loop 已支持多轮 tool call，ReAct 本质上是加 `think` 工具 + 优化 prompt。

### 变更清单

| 文件 | 操作 | 行数 | 说明 |
|------|------|------|------|
| `tools/think.h/cpp` | 新增 | ~30 | Think 工具: 不执行, 记录推理 |
| `server/src/server.cpp` | 修改 | ~15 | `run_acp_loop`: think 工具特殊处理 |
| `context_source.cpp` | 修改 | ~10 | `project_instructions`: 注入 ReAct prompt |
| **总计** | | **~55** | |

### 1. Think 工具

```cpp
class ThinkTool : public Tool {
    ToolSchema schema() const override {
        return {"think", "Record your reasoning step. "
                "Use this to think through complex problems before acting.",
                {{"type","object"}, {"properties", {
                    {"thought", {{"type","string"}, {"description","Your reasoning"}}}
                }}, {"required", {"thought"}}}};
    }
    Permission default_permission() const override { return Permission::Allow; }
    ToolResult execute(const ToolCall& call) override {
        auto thought = call.arguments.value("thought", "");
        LOG_DEBUG("LLM thought: {}", thought);
        return {call.id, true, "Thought recorded."};
    }
};
```

### 2. ReAct System Prompt

```
## ReAct Pattern

You MUST follow this cycle when solving complex problems:

1. **Thought**: Use the `think` tool to reason about what to do next.
   - Analyze the problem
   - Plan your approach
   - Decide which tool to use

2. **Action**: Call a tool (read, write, edit, bash, glob, grep).

3. **Observation**: The tool result will be shown.

4. **Repeat** Thought → Action → Observation until done.

5. **Final Answer**: When you have the solution, respond directly.

Guidelines:
- ALWAYS use `think` before calling any other tool
- If a tool fails, use `think` to analyze the error and plan recovery
- Be thorough — explore the codebase fully before making changes
- Verify your changes with `bash` before declaring done
```

### 3. ACP Loop 改动

```cpp
// run_acp_loop 中 tool 执行部分
for (auto& call : call_list) {
    // think 工具不需要权限检查，静默执行
    if (call.name == "think") {
        auto result = tool_registry_.execute(call);
        LOG_DEBUG("ReAct: {}", call.arguments.value("thought", ""));
        continue;  // 不推送到 SSE，不加入消息历史
    }

    // 原有 tool 执行逻辑...
    auto perm = tool_registry_.check_permission(call.name);
    // ...
}
```

### 4. 工作流对比

```
当前 ACP Loop:                          ReAct ACP Loop:
──────────────                         ──────────────
LLM → [直接调用工具]                     LLM → think("分析问题")
      → tool_result                          → read("file.cpp")
      → LLM 继续                                   → observation
                                                → think("修复方案")
                                                → edit("file.cpp")
                                                → observation
                                                → think("验证")
                                                → bash("cmake --build")
                                                → observation
                                                → Final Answer
```

### 5. 无需新增依赖

`ThinkTool` 纯内存操作，零系统调用。全部改动 ~55 行，不涉及 vcpkg/CMake。

---

## 二、RAG (Retrieval-Augmented Generation)

### 双层架构

```
┌────────────────────────────────────────────────────────────┐
│                      RAG 系统                               │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  Layer 1: Lexical Search (内置, 零依赖)                      │
│  ├─ SQLite FTS5 全文索引                                    │
│  ├─ BM25 相关性排序                                         │
│  └─ 使用: glob → 扫描文件 → FTS 索引 → 查询                   │
│                                                            │
│  Layer 2: Semantic Search (可选, 需 embedding API)           │
│  ├─ 利用现有 Provider (OpenAI/DeepSeek) 做 embedding         │
│  ├─ SQLite 存储向量 (BLOB)                                   │
│  └─ 余弦相似度暴力计算 (项目级文件数不多, 无需 FAISS)           │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### 技术选型

| 组件 | 技术 | 理由 |
|------|------|------|
| 全文索引 | **SQLite FTS5** | 已用 SQLite，内置 FTS5，零新依赖 |
| 分词 | **内置 porter tokenizer** | 英文代码分词足够 |
| 向量存储 | **SQLite BLOB** | 项目级文件数 < 10K，暴力计算 CPU 无压力 |
| Embedding | **现有 Provider** (DeepSeek/OpenAI) | 复用已有 HTTP 客户端 |
| 分块 | **函数/类级别** | regex 匹配 `^class\s+\w+` `^\w+::` |

### 数据流

```
Phase 1: 构建索引
─────────────────
  glob("**/*.{cpp,h,py,js,ts,rs}")
    │
    ▼
  for each file:
    chunker.split(file_content)
      │
      ├─ 分割策略: 函数/类边界
      ├─ 块大小: 200-2000 行
      └─ 元数据: {file, start_line, end_line, type}
        │
        ▼
    FTS5 INSERT (content, metadata)
    [可选] Embedding → BLOB

Phase 2: 查询
─────────────
  user query ("authentication 怎么实现的？")
    │
    ├─ Layer 1 (FTS5): SELECT * FROM chunks WHERE content MATCH ?
    │   └─ BM25 排序, top K=5
    │
    ├─ Layer 2 (可选, embedding):
    │   ├─ Provider.embed(query) → vector
    │   ├─ SELECT * FROM chunks
    │   ├─ cos_similarity(query_vec, each chunk_vec)
    │   └─ top K=5
    │
    └─ Merge + dedup → top K=5 chunks
        │
        ▼
    inject into System Context as <retrieved_chunks>...</retrieved_chunks>
```

### SQLite FTS5 表

```sql
CREATE VIRTUAL TABLE chunks USING fts5(
    file_path,       -- unindexed
    chunk_id,        -- unindexed
    content,         -- indexed (全文搜索)
    metadata,        -- unindexed (JSON: {file, start, end, type, hash})
    tokenize='porter unicode61'
);
```

### 代码分块策略

```cpp
struct Chunk {
    std::string file;
    int start_line, end_line;
    std::string type;   // "function" | "class" | "namespace" | "header" | "other"
    std::string content;
    std::string hash;   // 文件内容 hash (用于增量更新)
};

class Chunker {
    std::vector<Chunk> split(const std::string& file_path,
                              const std::string& content);
private:
    // 正则匹配函数/类边界
    // C++: ^\w+::\w+|^class\s+\w+|^struct\s+\w+
    // Python: ^def\s+\w+|^class\s+\w+
    // JS/TS: ^function\s+\w+|^class\s+\w+|^export\s+
};
```

### 接口

```cpp
// rag_store.h
class RagStore {
public:
    RagStore(const std::string& db_path);

    // 构建/重建索引
    void build_index(const std::string& project_root,
                      const std::vector<std::string>& patterns = {"**/*.{cpp,h,hpp}"});

    // 全文搜索 (Layer 1)
    std::vector<Chunk> search(const std::string& query, int top_k = 5);

    // 语义搜索 (Layer 2, 可选)
    std::vector<Chunk> semantic_search(const std::vector<float>& query_embedding,
                                        int top_k = 5);

    // 混合搜索
    std::vector<Chunk> hybrid_search(const std::string& query,
                                      std::optional<std::vector<float>> embedding = std::nullopt,
                                      int top_k = 5);

private:
    sqlite3* db_;
    Chunker chunker_;
};
```

### 新增工具

| Tool | 功能 | 说明 |
|------|------|------|
| `rag_search` | 搜索项目知识 | FTS5 + 可选 embedding |
| `rag_index` | 构建/重建索引 | 扫描项目文件, 分块, 入库 |

### 与 System Context 集成

```cpp
// 新增 context_source: rag
ContextSource rag_source(RagStore& rag) {
    return {"rag_knowledge", "...",
        .loader = [&rag]() -> ContextValue {
            return {.available = false}; // 默认不注入, 由 LLM 按需调用 rag_search
        }
    };
}
```

LLM 通过工具调用 `rag_search("authentication")` 按需检索，避免所有请求都注入大段上下文。

### 文件清单

| 文件 | 操作 | 行数 |
|------|------|------|
| `packages/llm/src/rag_store.h` | 新增 | ~40 |
| `packages/llm/src/rag_store.cpp` | 新增 | ~200 |
| `packages/llm/src/chunker.h/cpp` | 新增 | ~80 |
| `packages/llm/src/tools/rag_tools.h/cpp` | 新增 | ~60 |
| `packages/server/src/server.cpp` | 修改 (注册+索引) | ~10 |
| **总计** | | **~390** |

### 依赖变化

```
零新增 vcpkg 依赖
  - SQLite FTS5: 系统 sqlite3 已包含
  - 分词: FTS5 内置 porter tokenizer
  - 无 FAISS / Qdrant / ChromaDB
```

### MVP 路线

| 步骤 | 内容 | 复杂度 |
|------|------|--------|
| 1 | SQLite FTS5 表 + `build_index()` | 低 |
| 2 | `Chunker` (函数/类级分割) | 低 |
| 3 | `rag_search` 工具 (FTS5 BM25) | 低 |
| 4 | `rag_index` 工具 (重建索引) | 低 |
| 5 | (可选) Embedding 语义搜索 | 中 |

---

## 三、总体 Phase 路线

| Phase | 版本 | 交付 |
|------|------|------|
| 1 | v0.1.0 | 单体 CLI + 非流式 LLM + TOML |
| 2 | v0.2.0 | C/S REST + Session |
| 3 | v0.3.0 | ACP + SSE 实时流式 |
| 3.1 | v0.3.1 | 多 Provider + 日志 + SSL |
| 4 | v0.4.0 | Tool Registry + 6 工具 + ACP 多轮 loop |
| 5 | v0.5.0 | SQLite 持久化 + System Context (6 sources) |
| 6 | v0.6.0 | TUI (FTXUI) + Anthropic Provider |
| **7** | **v0.7.0** | **ReAct + RAG (本文档)** |
| 8 | v0.8.0 | Plugin 系统 (C ABI) |
