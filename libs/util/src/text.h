#pragma once

#include <string>

namespace codis::util {

// 生成会话标题：取首条 user 消息，去首尾空白、换行折叠、UTF-8 安全截断
std::string make_session_title(const std::string& content, size_t max_chars = 30);

// 生成 8 位十六进制短 ID（随机）
std::string gen_short_id();

} // namespace codis::util
