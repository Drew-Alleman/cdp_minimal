#pragma once

#include <string>
#include <cstdint>
#include <variant>

namespace cdp {

    struct MouseEvent {
        int x, y;
        std::string type; // "mousedown", "mousemove", etc.
        std::string url;
    };

    struct KeyboardEvent {
        std::string key;
        bool repeat;
        std::string url;
    };

    struct ActivityEvent {
        std::string type;
        std::string url;
        std::string title;
        std::int64_t timestamp = 0;
    };

    struct TabInfo {
        std::string target;
        std::string url;
        std::string title;
        bool focused = false;
        std::int64_t last_active = 0;
    };

    using InputEvent = std::variant<MouseEvent, KeyboardEvent>;

} // namespace cdp
