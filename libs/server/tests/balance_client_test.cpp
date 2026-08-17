// BalanceClient 纯函数单测：URL 推导与响应解析（HTTP 层不做真实网络调用）。

#include "balance_client.h"

#include <gtest/gtest.h>

using namespace codis;

TEST(BalanceClient, BuildUrlStripsV1) {
    EXPECT_EQ(BalanceClient::build_balance_url("https://api.deepseek.com/v1"),
              "https://api.deepseek.com/user/balance");
    EXPECT_EQ(BalanceClient::build_balance_url("https://api.deepseek.com/v1/"),
              "https://api.deepseek.com/user/balance");
    EXPECT_EQ(BalanceClient::build_balance_url("https://api.deepseek.com"),
              "https://api.deepseek.com/user/balance");
    EXPECT_EQ(BalanceClient::build_balance_url("https://api.deepseek.com/"),
              "https://api.deepseek.com/user/balance");
    EXPECT_EQ(BalanceClient::build_balance_url("https://host.example.com/custom/path"),
              "https://host.example.com/custom/path/user/balance");
    EXPECT_EQ(BalanceClient::build_balance_url(""),
              "/user/balance");
}

TEST(BalanceClient, ParseResponseOk) {
    auto j = BalanceClient::parse_balance_response(
        "deepseek", 200,
        R"({"balance_infos": [{"total_balance": "1.00"}], "is_available": true})");
    EXPECT_EQ(j["provider"], "deepseek");
    EXPECT_EQ(j["balance"]["is_available"], true);
    EXPECT_EQ(j["balance"]["balance_infos"][0]["total_balance"], "1.00");
}

TEST(BalanceClient, ParseResponseNon200Throws) {
    EXPECT_THROW(BalanceClient::parse_balance_response("deepseek", 400, R"({"error":"bad"})"),
                 std::runtime_error);
    EXPECT_THROW(BalanceClient::parse_balance_response("deepseek", 500, "oops"),
                 std::runtime_error);
}

TEST(BalanceClient, ParseResponseInvalidJsonThrows) {
    EXPECT_THROW(BalanceClient::parse_balance_response("deepseek", 200, "not json"),
                 std::runtime_error);
}