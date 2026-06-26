// detail/base64.h
#pragma once
#include <string>
#include <vector>

namespace cdp::detail {
    inline std::string base64Decode(const std::string& in) {
        static const std::string chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        static const std::vector<int> table = [] {
            std::vector<int> t(256, -1);
            for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(chars[i])] = i;
            return t;
            }();

        std::string out;
        int val = 0, valb = -8;
        for (unsigned char c : in) {
            if (table[c] == -1) continue;
            val = (val << 6) + table[c];
            valb += 6;
            if (valb >= 0) {
                out.push_back(char((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }
} // namespace cdp::detail
