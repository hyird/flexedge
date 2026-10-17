#pragma once
#include <array>
#include <string_view>

#include "common/cache_policy.h"

namespace flexedge::node {
inline bool cache_content_type_matches(std::string_view target, std::string_view content_type) {
    target = target.substr(0, target.find('?'));
    const auto dot = target.rfind('.');
    if (dot == std::string_view::npos || target.find('/', dot) != std::string_view::npos) {
        return true;
    }
    const auto extension = target.substr(dot);
    const auto type = flexedge::route_policy::trim_ows(content_type.substr(0, content_type.find(';')));
    if (type.empty() || flexedge::route_policy::ascii_equal(type, "application/octet-stream")) {
        return true;
    }
    struct mime final {
        std::string_view extension_, types_;
    };
    constexpr std::array mappings{
        mime{".js", "application/javascript|text/javascript|application/x-javascript"}, mime{".css", "text/css"}, mime{".csv", "text/csv|application/csv"},
        mime{".png", "image/png|image/webp|image/avif"}, mime{".jpg", "image/jpeg|image/webp|image/avif"}, mime{".jpeg", "image/jpeg|image/webp|image/avif"},
        mime{".gif", "image/gif|image/webp|image/avif|video/webm"}, mime{".webp", "image/webp|image/avif"}, mime{".avif", "image/avif"},
        mime{".svg", "image/svg+xml"}, mime{".svgz", "image/svg+xml"}, mime{".ico", "image/x-icon|image/vnd.microsoft.icon"},
        mime{".bmp", "image/bmp|image/x-ms-bmp"}, mime{".tif", "image/tiff"}, mime{".tiff", "image/tiff"}, mime{".pict", "image/x-pict"},
        mime{".woff", "font/woff|application/font-woff"}, mime{".woff2", "font/woff2"}, mime{".ttf", "font/ttf|application/x-font-ttf"}, mime{".otf", "font/otf|application/x-font-opentype"}, mime{".eot", "application/vnd.ms-fontobject"},
        mime{".pdf", "application/pdf"}, mime{".mp3", "audio/mpeg"}, mime{".mp4", "video/mp4|audio/mp4"}, mime{".webm", "video/webm|audio/webm"},
        mime{".avi", "video/x-msvideo|video/avi"}, mime{".mkv", "video/x-matroska"}, mime{".ogg", "audio/ogg|video/ogg|application/ogg"}, mime{".flac", "audio/flac|audio/x-flac"},
        mime{".mid", "audio/midi|audio/x-midi"}, mime{".midi", "audio/midi|audio/x-midi"}, mime{".pls", "audio/x-scpls|application/pls+xml"},
        mime{".zip", "application/zip|application/x-zip-compressed"}, mime{".7z", "application/x-7z-compressed"}, mime{".rar", "application/vnd.rar|application/x-rar-compressed"},
        mime{".gz", "application/gzip|application/x-gzip"}, mime{".tar", "application/x-tar"}, mime{".bz", "application/x-bzip"}, mime{".bz2", "application/x-bzip2"}, mime{".zst", "application/zstd"},
        mime{".doc", "application/msword"}, mime{".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        mime{".xls", "application/vnd.ms-excel"}, mime{".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        mime{".ppt", "application/vnd.ms-powerpoint"}, mime{".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        mime{".apk", "application/vnd.android.package-archive"}, mime{".jar", "application/java-archive|application/x-java-archive"}, mime{".class", "application/java-vm"},
        mime{".swf", "application/x-shockwave-flash"}, mime{".eps", "application/postscript"}, mime{".ps", "application/postscript"},
        mime{".dmg", "application/x-apple-diskimage"}, mime{".iso", "application/x-iso9660-image"}, mime{".exe", "application/vnd.microsoft.portable-executable|application/x-msdownload"},
        mime{".ejs", "text/html|application/javascript|text/javascript"}, mime{".bin", "application/octet-stream"},
        mime{".mjs", "application/javascript|text/javascript|application/x-javascript"}, mime{".wasm", "application/wasm"},
        mime{".apng", "image/apng|image/png"}, mime{".jxl", "image/jxl"}, mime{".heic", "image/heic|image/heif"}, mime{".heif", "image/heif|image/heic"}, mime{".jfif", "image/jpeg"}, mime{".cur", "image/x-icon|image/vnd.microsoft.icon"},
        mime{".ttc", "font/collection|application/x-font-ttf"}, mime{".otc", "font/collection"}, mime{".sfnt", "font/sfnt|application/font-sfnt"},
        mime{".aac", "audio/aac|audio/x-aac"}, mime{".wav", "audio/wav|audio/x-wav|audio/vnd.wave"}, mime{".m4a", "audio/mp4|audio/x-m4a"}, mime{".opus", "audio/opus|audio/ogg"}, mime{".oga", "audio/ogg"}, mime{".weba", "audio/webm"},
        mime{".m4v", "video/mp4|video/x-m4v"}, mime{".mov", "video/quicktime"}, mime{".mpeg", "video/mpeg"}, mime{".mpg", "video/mpeg"}, mime{".ogv", "video/ogg"},
        mime{".m3u8", "application/vnd.apple.mpegurl|application/x-mpegurl|audio/mpegurl"}, mime{".mpd", "application/dash+xml|application/xml"}, mime{".m4s", "video/iso.segment|video/mp4|audio/mp4"},
        mime{".xz", "application/x-xz"}, mime{".txz", "application/x-xz"}, mime{".tgz", "application/gzip|application/x-gzip|application/x-compressed-tar"},
        mime{".deb", "application/vnd.debian.binary-package|application/x-debian-package"}, mime{".rpm", "application/x-rpm"}, mime{".cab", "application/vnd.ms-cab-compressed"},
        mime{".msi", "application/x-msi|application/x-msdownload"}, mime{".msix", "application/msix"}, mime{".msixbundle", "application/msixbundle"}, mime{".appx", "application/appx"}, mime{".appxbundle", "application/appxbundle"},
        mime{".epub", "application/epub+zip"}, mime{".rtf", "application/rtf|text/rtf"}, mime{".odp", "application/vnd.oasis.opendocument.presentation"}, mime{".ods", "application/vnd.oasis.opendocument.spreadsheet"}, mime{".odt", "application/vnd.oasis.opendocument.text"}};
    for (const auto& entry : mappings) {
        if (!flexedge::route_policy::ascii_equal(extension, entry.extension_)) {
            continue;
        }
        auto values = entry.types_;
        while (!values.empty()) {
            const auto separator = values.find('|');
            if (flexedge::route_policy::ascii_equal(type, values.substr(0, separator))) {
                return true;
            }
            values = separator == std::string_view::npos ? std::string_view{} : values.substr(separator + 1);
        }
        return false;
    }
    return true;
}
}  // namespace flexedge::node
