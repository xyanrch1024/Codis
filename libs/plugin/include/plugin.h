#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define CODIS_PLUGIN_API_VERSION 1

// ---- 日志级别 ----
enum CodisLogLevel {
    CODIS_LOG_TRACE = 0,
    CODIS_LOG_DEBUG = 1,
    CODIS_LOG_INFO  = 2,
    CODIS_LOG_WARN  = 3,
    CODIS_LOG_ERROR = 4,
};

// ---- 工具注册回调 ----
// execute_fn: 接收 JSON 参数，返回 JSON 结果（调用者需 free）
typedef char* (*codis_tool_execute_fn)(const char* arguments_json, void* ctx);

// ---- 上下文加载回调 ----
typedef char* (*codis_context_loader_fn)(void* ctx);

// ---- 核心 API 函数表 ----
struct CodisAPI;
typedef struct CodisAPI CodisAPI;

struct CodisAPI {
    int version;

    void (*log)(int level, const char* msg);

    void (*register_tool)(const char* name,
                          const char* description,
                          const char* parameters_json,
                          codis_tool_execute_fn execute,
                          void* ctx);

    void (*register_context)(const char* key,
                             const char* description,
                             codis_context_loader_fn loader,
                             void* ctx);
};

// ---- 插件入口（每个 .so 必须导出）----
typedef int  (*codis_plugin_init_fn)(const struct CodisAPI* api, const char* config_json);
typedef void (*codis_plugin_shutdown_fn)(void);

#ifdef __cplusplus
}
#endif
