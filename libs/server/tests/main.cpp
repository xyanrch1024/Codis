// 服务端纯逻辑单测入口（无网络无终端）。
// 运行: ctest -R server_logic  （或直接执行 codis_server_test）

#include "test_util.h"

#include <iostream>

void run_session_store_tests();
void run_context_utils_tests();
void run_tool_call_parse_tests();
void run_protocol_tests();

int main() {
    run_session_store_tests();
    run_context_utils_tests();
    run_tool_call_parse_tests();
    run_protocol_tests();

    int n = testutil::failures();
    if (n == 0) {
        std::cout << "server_logic: all tests passed\n";
        return 0;
    }
    std::cerr << "server_logic: " << n << " failure(s)\n";
    return 1;
}