#pragma once

#include <array>
#include <utility>

#include "common/cache_policy.h"

namespace flexedge::cache {

// Cloudflare standard cache eligibility, independent of node storage budgets.
[[nodiscard]] inline rule default_static_rule() {
    return {.id_ = "cache-static",
        .name_ = "静态资源",
        .patterns_ = {".7z", ".avi", ".avif", ".apk", ".bin", ".bmp", ".bz", ".bz2",
            ".class", ".css", ".csv", ".doc", ".docx", ".dmg", ".ejs", ".eot", ".eps", ".exe",
            ".flac", ".gif", ".gz", ".ico", ".iso", ".jar", ".jpg", ".jpeg", ".js", ".mid",
            ".midi", ".mkv", ".mp3", ".mp4", ".ogg", ".otf", ".pdf", ".pict", ".pls", ".png",
            ".ppt", ".pptx", ".ps", ".rar", ".svg", ".svgz", ".swf", ".tar", ".tif", ".tiff",
            ".ttf", ".webm", ".webp", ".woff", ".woff2", ".xls", ".xlsx", ".zip", ".zst"}};
}

// Explicit static allowlist. Requests without a matching rule bypass cache.
[[nodiscard]] inline std::vector<rule> safe_static_rules() {
    auto assets = default_static_rule();
    std::erase(assets.patterns_, ".ejs");
    constexpr std::array additions{".mjs", ".wasm", ".apng", ".jxl", ".heic", ".heif", ".jfif", ".cur",
        ".ttc", ".otc", ".sfnt", ".aac", ".wav", ".m4a", ".opus", ".oga", ".weba", ".m4v", ".mov",
        ".mpeg", ".mpg", ".ogv", ".m3u8", ".mpd", ".m4s", ".xz", ".txz", ".tgz", ".deb", ".rpm",
        ".cab", ".msi", ".msix", ".msixbundle", ".appx", ".appxbundle", ".epub", ".rtf", ".odp", ".ods", ".odt"};
    for (const auto extension : additions) {
        assets.patterns_.emplace_back(extension);
    }
    auto& settings = assets.settings_.emplace();
    settings.enabled_ = {"edge", "browser", "armor", "origin"};
    settings.armor_ = true;
    assets.browser_ttl_mode_ = "respect_origin";
    return {std::move(assets)};
}

}  // namespace flexedge::cache
