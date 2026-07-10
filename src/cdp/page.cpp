#include "cdp/page.h"
#include "detail/channel.h"
#include "detail/base64.h"

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace cdp {

    Page::Page(std::shared_ptr<detail::Channel> channel,
        std::string target_id,
        std::string session_id)
        : channel_(std::move(channel))
        , target_id_(std::move(target_id))
        , session_id_(std::move(session_id))
    {
        if (channel_ && !session_id_.empty()) {
            auto ch = channel_;       
            auto sid = session_id_;
            session_guard_ = std::shared_ptr<void>(nullptr, [ch, sid](void*) {
                try {
                    ch->result_of("Target.detachFromTarget",
                        json{ {"sessionId", sid} }, "");
                }
                catch (...) {
                }
                });
        }
    }

    bool Page::valid() const noexcept {
        return static_cast<bool>(channel_);
    }

    Result<std::string> Page::evaluate(const std::string& js) {
        if (!channel_) {
            return Error{ Errc::not_connected, "Page handle is invalid (tab may have closed)" };
        }

        try {
            json params = {
                {"expression", js},
                {"returnByValue", true},
                {"awaitPromise", true}
            };

            json result = channel_->result_of("Runtime.evaluate", params, session_id_);

            if (result.contains("exceptionDetails")) {
                const auto& ex = result["exceptionDetails"];
                std::string errorText = ex.value("text", "Unknown JavaScript error");

                if (ex.contains("exception") && ex["exception"].contains("description")) {
                    errorText = ex["exception"]["description"].get<std::string>();
                }

                return Error{ Errc::protocol, "JavaScript error: " + errorText };
            }

            const json& r = result.at("result");

            if (r.value("type", "") == "string") {
                return r.value("value", std::string{});
            }

            if (r.contains("value")) {
                return r["value"].dump();
            }

            return std::string{};

        }
        catch (const detail::TimeoutError& e) {
            return Error{ Errc::timeout, e.what() };
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response,
                "Failed to execute JavaScript: " + std::string(e.what()) };
        }
    }

    Result<void> Page::screenshot(const std::string& path) {
        if (!channel_) {
            return Error{ Errc::not_connected, "invalid page handle" };
        }

        try {
            json params = { {"format", "png"} };
            json result = channel_->result_of("Page.captureScreenshot", params, session_id_);

            if (!result.contains("data") || result["data"].is_null()) {
                return Error{ Errc::bad_response,
                    "Failed to capture screenshot. The page might be blank, the window is not visible, or protected (e.g. chrome:// pages)." };
            }

            std::string bytes = cdp::detail::base64Decode(result["data"].get<std::string>());

            std::ofstream out(path, std::ios::binary);
            if (!out) {
                return Error{ Errc::io, "cannot open for writing: " + path };
            }

            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out.good()) {
                return Error{ Errc::io, "write failed: " + path };
            }

            return {};
        }
        catch (const detail::TimeoutError& e) {
            return Error{ Errc::timeout, e.what() };
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<std::vector<Cookie>> Page::getCookies(const std::vector<std::string>& urls) {
        if (!channel_) {
            return Error{ Errc::not_connected, "invalid page handle" };
        }

        (void)urls;

        try {
            json res = channel_->result_of("Storage.getCookies", json::object(), session_id_);

            std::vector<Cookie> cookies;
            for (const auto& c : res.at("cookies")) {
                cookies.push_back({
                    c.value("name", ""),
                    c.value("value", ""),
                    c.value("domain", ""),
                    c.value("path", ""),
                    c.value("expires", -1.0),
                    c.value("httpOnly", false),
                    c.value("secure", false),
                    false,
                    c.value("sameSite", "")
                    });
            }
            return cookies;
        }
        catch (const detail::TimeoutError& e) {
            return Error{ Errc::timeout, e.what() };
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<std::string> Page::getURL() {
        return evaluate("window.location.href");
    }

    Result<void> Page::bringToFront() {
        if (!channel_) {
            return Error{ Errc::not_connected, "invalid page handle" };
        }

        try {
            channel_->result_of("Page.bringToFront", json::object(), session_id_);
            return {};
        }
        catch (const detail::TimeoutError& e) {
            return Error{ Errc::timeout, e.what() };
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<std::string> Page::getContent() {
        if (!channel_) {
            return Error{ Errc::not_connected, "invalid page handle" };
        }

        try {
            json params = {
                {"expression", "document.documentElement.outerHTML"},
                {"returnByValue", true}
            };

            json result = channel_->result_of("Runtime.evaluate", params, session_id_);

            if (result.contains("exceptionDetails")) {
                const auto& ex = result["exceptionDetails"];
                std::string msg = ex.value("text", "Unknown error while getting page content");
                return Error{ Errc::protocol, "JavaScript error: " + msg };
            }

            const json& r = result.at("result");

            if (r.contains("value")) {
                return r["value"].get<std::string>();
            }

            return std::string{};

        }
        catch (const detail::TimeoutError& e) {
            return Error{ Errc::timeout, e.what() };
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response,
                "Failed to get page content: " + std::string(e.what()) };
        }
    }
    Result<void> Page::grantClipboardPermission() {
        if (!channel_) {
            return Error{ Errc::not_connected, "invalid page handle" };
        }

        try {
            auto originRes = evaluate(R"(window.location.origin)");
            if (!originRes) {
                return Error{ Errc::bad_response,
                    "Failed to read page origin: " + originRes.error().message };
            }

            std::string origin = originRes.value();

            // evaluate() may return the JS string quoted ("https://example.com") or the
            // literal null — reject anything that isn't a real origin.
            if (origin.empty() || origin == "null" || origin == "\"null\"") {
                return Error{ Errc::bad_response,
                    "Page has an opaque origin (about:blank/data:/chrome:) — "
                    "navigate to a real http(s) origin before granting clipboard permission" };
            }

            json params = {
                {"origin", origin},
                {"permissions", json::array({ "clipboardReadWrite", "clipboardSanitizedWrite" })}
            };
            channel_->result_of("Browser.grantPermissions", params, session_id_);;

            return {};
        }
        catch (const detail::TimeoutError& e) {
            return Error{ Errc::timeout, e.what() };
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, "Failed to grant clipboard permission: " + std::string(e.what()) };
        }
    }

    Result<std::string> Page::getClipboardText() {

        if (!channel_) {
            return Error{ Errc::not_connected, "invalid page handle" };
        }

        grantClipboardPermission();

        // Reuse your existing evaluate method
        return evaluate(R"(navigator.clipboard.readText())");
    }

    Result<void> Page::setClipboardText(const std::string& text) {
        if (!channel_) {
            return Error{ Errc::not_connected, "invalid page handle" };
        }

        // 1. Grant permission (safe to call every time)
        auto perm = grantClipboardPermission();
        if (!perm) return perm.error();

        // 2. Write to the real system clipboard
        std::string js = "navigator.clipboard.writeText(" + json(text).dump() + ")";

        auto res = evaluate(js);
        if (!res) return res.error();

        return {};
    }

} // namespace cdp
