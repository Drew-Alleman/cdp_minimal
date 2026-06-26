#pragma once

#include <memory>
#include <string>
#include <vector>

#include "result.h"
#include "cookie.h"

namespace cdp {

    namespace detail { class Channel; } 
    class Page {
    public:
        Page() = default;
        Page(std::shared_ptr<detail::Channel> channel,
            std::string target_id,
            std::string session_id);

        Result<std::string> evaluate(const std::string& js);
        Result<void>        screenshot(const std::string& path);
        Result<std::vector<Cookie>> getCookies(const std::vector<std::string>& urls = {});
        Result<std::string> getContent();

        Result<std::string> getURL();
        Result<void>        bringToFront();

        bool valid() const noexcept;

    private:
        std::shared_ptr<detail::Channel> channel_;
        std::string target_id_;
        std::string session_id_;
        std::shared_ptr<void> session_guard_;  // detaches the CDP session when the last Page copy dies
    };

} // namespace cdp
