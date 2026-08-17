#include "balance_client.h"
#include "log.h"

#include <httplib.h>

#include <string>

namespace codis {

std::string BalanceClient::build_balance_url(const std::string& base_url) {
    std::string balance_url = base_url;
    // 对于 DeepSeek: https://api.deepseek.com -> https://api.deepseek.com/user/balance
    // 尝试去除末尾的 /v1 路径段
    if (balance_url.ends_with("/v1")) {
        balance_url = balance_url.substr(0, balance_url.size() - 3);
    } else if (balance_url.ends_with("/v1/")) {
        balance_url = balance_url.substr(0, balance_url.size() - 4);
    }
    // 确保末尾没有斜杠
    while (!balance_url.empty() && balance_url.back() == '/')
        balance_url.pop_back();
    return balance_url + "/user/balance";
}

json BalanceClient::parse_balance_response(const std::string& provider_name, int status,
                                           const std::string& body) {
    if (status != 200) {
        throw std::runtime_error("Balance API returned HTTP " + std::to_string(status)
                                 + ": " + body.substr(0, 200));
    }
    try {
        auto j = json::parse(body);
        json result;
        result["provider"] = provider_name;
        result["balance"] = j;
        return result;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("Failed to parse balance response: " + std::string(e.what()));
    }
}

json BalanceClient::query(const AppConfig& config, const std::string& provider_name) {
    // 查找 provider 配置
    const ProviderConfig* target = config.provider_for(provider_name);
    if (!target) {
        throw std::runtime_error("Provider '" + provider_name + "' not found in config");
    }

    if (target->api_key.empty()) {
        throw std::runtime_error("No API key for provider '" + provider_name + "'");
    }

    std::string balance_endpoint = build_balance_url(target->base_url);
    LOG_DEBUG("querying balance for '{}' at {}", provider_name, balance_endpoint);

    // 解析 host 和 path
    std::string url_part;
    bool use_ssl = false;

    if (balance_endpoint.starts_with("https://")) {
        use_ssl = true;
        url_part = balance_endpoint.substr(8);
    } else if (balance_endpoint.starts_with("http://")) {
        url_part = balance_endpoint.substr(7);
    } else {
        throw std::runtime_error("Invalid URL: " + balance_endpoint);
    }

    std::string host, path;
    auto slash_pos = url_part.find('/');
    if (slash_pos != std::string::npos) {
        host = url_part.substr(0, slash_pos);
        path = url_part.substr(slash_pos);
    } else {
        host = url_part;
        path = "/";
    }

    httplib::Client client((use_ssl ? "https://" : "http://") + host);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(10, 0);

    httplib::Headers headers = {
        {"Authorization", "Bearer " + target->api_key},
        {"Accept", "application/json"}
    };

    auto http_res = client.Get(path, headers);
    if (!http_res) {
        throw std::runtime_error("HTTP request failed: " + httplib::to_string(http_res.error()));
    }

    return parse_balance_response(provider_name, http_res->status, http_res->body);
}

} // namespace codis