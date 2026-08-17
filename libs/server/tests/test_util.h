#pragma once

// 轻量测试辅助：CHECK 宏 + 失败计数（与 libs/tui/tests/ui_logic_test.cpp 同风格，
// 零测试框架依赖）。多个测试 TU 共享，main.cpp 汇总失败数作为退出码。

#include <iostream>

namespace testutil {

inline int& failures() {
    static int n = 0;
    return n;
}

} // namespace testutil

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++testutil::failures();                                          \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond \
                      << "\n";                                               \
        }                                                                    \
    } while (0)
