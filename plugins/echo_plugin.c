#include "plugin.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static char* echo_execute(const char* args_json, void* ctx) {
    (void)ctx;
    char* result = (char*)malloc(256);
    snprintf(result, 256, "{\"echo\": \"plugin received %zu bytes\"}",
             strlen(args_json));
    return result;
}

int plugin_init(const CodisAPI* api, const char* config_json) {
    (void)config_json;
    api->log(CODIS_LOG_INFO, "echo plugin loaded");

    api->register_tool(
        "echo_plugin",
        "Echo plugin — echoes back arguments",
        "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}},\"required\":[\"message\"]}",
        echo_execute,
        NULL
    );

    return 0;
}

void plugin_shutdown(void) {
}
