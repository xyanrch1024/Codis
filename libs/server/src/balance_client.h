// BalanceClient — provider 余额查询。
// URL 推导与响应解析为纯静态函数（可单测）；httplib GET 仅实例方法使用。

#pragma once

#include "config.h"

#include <nlohmann/json.hpp>

#include <string>

namespace codis {

using json = nlohmann::json;

class BalanceClient {
public:
    // 由 provider base_url 推导余额端点：剥离末尾 /v1 路径段，拼接 /user/balance
    static std::string build_balance_url(const std::string& base_url);
    // 解析余额响应为 {provider, balance}；非 200 或坏 JSON 抛 std::runtime_error
    static json parse_balance_response(const std::string& provider_name, int status,
                                       const std::string& body);

    // 查询 provider 余额（含 API key 校验与 HTTP 请求）；失败抛 std::runtime_error
    json query(const AppConfig& config, const std::string& provider_name);
};

} // namespace codis