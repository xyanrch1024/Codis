#include "text.h"

#include <iomanip>
#include <random>
#include <sstream>

namespace codis::util {

std::string make_session_title(const std::string& content, size_t max_chars) {
    std::string out;
    size_t chars = 0, i = 0;
    while (i < content.size() && chars < max_chars) {
        unsigned char c = (unsigned char)content[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > content.size()) break;
        std::string ch = content.substr(i, len);
        if (ch == "\n" || ch == "\r" || ch == "\t") ch = " ";
        out += ch;
        chars++;
        i += len;
    }
    auto b = out.find_first_not_of(" \t");
    auto e = out.find_last_not_of(" \t");
    if (b == std::string::npos) return "";
    return out.substr(b, e - b + 1);
}

std::string gen_short_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 8; i++) ss << dis(gen);
    return ss.str();
}

} // namespace codis::util
