#include "cdp/cookie.h"

#include <algorithm>
#include <regex>

namespace cdp {

namespace {

bool domain_at_or_under(std::string cdom, const std::string& base) {
    if (!cdom.empty() && cdom.front() == '.') {
        cdom.erase(0, 1);
    }
    if (cdom == base) return true;

    const std::string dotted = "." + base;
    return cdom.size() > dotted.size() &&
           cdom.compare(cdom.size() - dotted.size(), dotted.size(), dotted) == 0;
}

} // namespace

Result<std::vector<Cookie>> select(const std::vector<Cookie>& cookies,
                                   const CookieFilter& f)
{
    std::optional<std::regex> name_re, value_re;

    try {
        if (f.name_pattern)  name_re.emplace(*f.name_pattern);
        if (f.value_pattern) value_re.emplace(*f.value_pattern);
    }
    catch (const std::regex_error& e) {
        return Error{ Errc::bad_response,
                      std::string("bad cookie filter regex: ") + e.what() };
    }

    // Collection-level gate
    for (const std::string& needed : f.require_names) {
        bool present = false;
        for (const auto& c : cookies) {
            if (c.name == needed) {
                present = true;
                break;
            }
        }
        if (!present) return std::vector<Cookie>{};
    }

    std::vector<Cookie> out;
    for (const auto& c : cookies) {
        if (f.secure_only && !c.secure) continue;
        if (f.http_only && !c.http_only) continue;
        if (name_re && !std::regex_search(c.name, *name_re)) continue;
        if (value_re && !std::regex_search(c.value, *value_re)) continue;
        if (f.domain && !domain_at_or_under(c.domain, *f.domain)) continue;

        out.push_back(c);
    }
    return out;
}

// Predefined filters
const CookieFilter CLAUDE = {
    R"(^(anthropic-device-id|sessionKey|activitySessionId)$)",
    std::nullopt,
    "claude.ai",
    true,
    false,
    { "anthropic-device-id", "sessionKeyLC", "lastActiveOrg", "sessionKey", "activitySessionId" }
};

const CookieFilter GITHUB = {
    R"(^(user_session|__Host-user_session_same_site|_device_id|dotcom_user|logged_in|_gh_sess)$)",
    std::nullopt,
    "github.com",
    false,
    false,
    { "user_session", "_gh_sess", "_device_id", "dotcom_user", "saved_user_sessions" }
};

const CookieFilter GOOGLE = {
    R"(^(SID|SSID|SIDCC|APISID|SAPISID|NID|AEC|LSID)$)",
    std::nullopt,
    "google.com",
    false,
    false,
    { "SID", "SSID", "SIDCC", "APISID", "SAPISID", "AEC", "SAPISID" }
};

const CookieFilter SLACK = {
    R"(^d$)",
    std::nullopt,
    "slack.com",
    false,
    false,
    { "d" }
};

const CookieFilter OKTA = {
    R"(^sid$)",
    std::nullopt,
    "okta.com",
    false,
    false,
    { "sid" }
};

const std::map<std::string, CookieFilter> KNOWN_COOKIES = {
    { "https://claude.ai",  CLAUDE  },
    { "https://github.com", GITHUB  },
    { "https://google.com", GOOGLE  },
    { "https://slack.com",           SLACK },
    { "https://okta.com",            OKTA }
};

} // namespace cdp

