#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "result.h"
#include "page.h"
#include "input_event.h"
#include "cookie.h"

namespace cdp {

    namespace detail { class Channel; }

    class Browser {
    public:
        Browser(std::string host = "127.0.0.1", uint16_t port = 9222);
        ~Browser();

        Browser(Browser&&) noexcept;
        Browser& operator=(Browser&&) noexcept;
        Browser(const Browser&) = delete;
        Browser& operator=(const Browser&) = delete;

        bool connect();
        bool isConnected() const noexcept;

        Result<Page> anyPage();
        Result<Page> currentPage();

        Result<void>                screenshot(const std::string& path);
        Result<std::vector<Cookie>> getCookies();
        Result<std::string> getURL();
        Result<std::string> getContent();
        Result<std::vector<Cookie>> getCookiesFromDomain(const std::string& domain);
        Result<std::vector<CookieMatch>> fetchGoodies();
        Result<void> grantPermissions(const std::vector<std::string>& permissions,
            const std::string& origin = "");
        Result<void> resetPermissions();
        Result<std::vector<std::string>> listWebcams();
        Result<void> saveWebcamPhoto(const std::string& path,
            int cameraIndex = 0);
        Result<std::string> _captureWebcamPhoto(int cameraIndex);
        Result<std::vector<std::string>> listMicrophones();
        Result<std::string> _captureAudio(int micIndex, int durationMs);
        Result<void> saveAudioClip(const std::string& path,
            int micIndex,
            int durationMs);

        Result<void> watchInput();
        Result<void> bringToFront();
        Result<std::string> readFileViaFileURI(const std::string& localPath);
        Result<void> navigate(const std::string& url);

        void         stopInput() noexcept;

        // ==================== Callback System ====================
        using CallbackId = std::uint32_t;

        using InputCallback = std::function<void(const InputEvent&)>;
        using ActivityCallback = std::function<void(const ActivityEvent&)>;

        using URLCallback = std::function<void(const std::string& url)>;

        CallbackId addOnNewURL(URLCallback cb);
        void       removeOnNewURL(CallbackId id);

        CallbackId addOnInput(InputCallback cb);
        CallbackId addOnActivity(ActivityCallback cb);

        void removeOnInput(CallbackId id);
        void removeOnActivity(CallbackId id);
        void clearAllCallbacks() noexcept;

        TabInfo currentTab() const;

        Result<void> minimizeAll();
        Result<void> unminimizeAll();
        Result<void> redirect(const std::string& url);

        Result<std::string> getClipboardText();
        Result<void> setClipboardText(const std::string& text);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace cdp
