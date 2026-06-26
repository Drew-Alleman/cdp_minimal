#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <chrono>

#include "result.h"

namespace cdp {

    struct Cookie {
        std::string name;
        std::string value;
        std::string domain;
        std::string path;
        double      expires = -1.0;
        bool        http_only = false;
        bool        secure = false;
        bool        session = false;
        std::string same_site;

        bool is_expired() const;
    };

    struct CookieFilter {
        std::optional<std::string> name_pattern;
        std::optional<std::string> value_pattern;
        std::optional<std::string> domain;
        bool secure_only = false;
        bool http_only = false;
        std::vector<std::string> require_names;
    };

    struct CookieMatch {
        std::string          url;
        const CookieFilter* filter;
        std::vector<Cookie>  cookies;
    };

    Result<std::vector<Cookie>> select(const std::vector<Cookie>& cookies, const CookieFilter& filter);

    // Predefined filters (you can keep these)
    extern const CookieFilter CLAUDE;
    extern const CookieFilter GITHUB;
    extern const CookieFilter GOOGLE;

    extern const std::map<std::string, CookieFilter> KNOWN_COOKIES;

} // namespace cdp
