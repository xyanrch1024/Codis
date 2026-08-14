#pragma once

#include <string>

#include <unistd.h>

namespace opencode {

// Base64 编码（RFC 4648），供 OSC52 剪贴板使用
inline std::string base64_encode(const std::string& in) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned a = static_cast<unsigned char>(in[i]);
        unsigned b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
        unsigned c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
        out += table[a >> 2];
        out += table[((a & 0x03) << 4) | (b >> 4)];
        out += i + 1 < in.size() ? table[((b & 0x0F) << 2) | (c >> 6)] : '=';
        out += i + 2 < in.size() ? table[c & 0x3F] : '=';
    }
    return out;
}

// 通过 OSC52 写入系统剪贴板（ESC ] 52 ; c ; <base64> BEL）。
// Windows Terminal / WSLg / kitty / alacritty / tmux 等现代终端均支持；
// 不支持的终端会忽略该序列，无副作用。
inline void copy_to_clipboard(const std::string& text) {
    std::string frame = "\x1b]52;c;" + base64_encode(text) + "\x07";
    (void)::write(STDOUT_FILENO, frame.data(), frame.size());
}

} // namespace opencode
