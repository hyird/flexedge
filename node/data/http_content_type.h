#pragma once

#include <string_view>

#include "common/route_policy.h"

namespace flexedge::node {

inline bool is_event_stream_content_type(std::string_view value) noexcept {
    return flexedge::route_policy::ascii_equal(
        flexedge::route_policy::trim_ows(value.substr(0, value.find(';'))), "text/event-stream");
}

inline bool accepts_event_stream(std::string_view value) noexcept {
    while (!value.empty()) {
        const auto separator = value.find(',');
        auto media_type = value.substr(0, separator);
        value =
            separator == std::string_view::npos ? std::string_view{} : value.substr(separator + 1);
        media_type = flexedge::route_policy::trim_ows(media_type.substr(0, media_type.find(';')));
        if (flexedge::route_policy::ascii_equal(media_type, "text/event-stream")) {
            return true;
        }
    }
    return false;
}

}  // namespace flexedge::node
