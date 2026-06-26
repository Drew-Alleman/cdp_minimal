#pragma once

#include <string>
#include <cstdint>

namespace cdp {

    struct InputEvent {
        std::string value;
        std::string tag;
        std::string id;
        std::string name;
        std::string url;
        std::string title;
        std::int64_t timestamp = 0;
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

} // namespace cdp
