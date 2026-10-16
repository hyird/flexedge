#pragma once

#include <ruvia/web/model.h>
#include <ruvia/web/model_object.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "service/features/attempt_trace/model.h"
#include "service/utils/json_text.h"
#include "service/utils/text.h"
#include "service/utils/utf8.h"

// Allow-listed, pretty-printed JSON excerpts of request and response bodies.
namespace service::attempt_trace {

// Paths use the syntax of outbound_policy.h.
struct json_excerpt_fields final {
    // Values kept; a kept container keeps its whole subtree.
    std::span<const std::string_view> allowed_{};
    // String values shown only as their size, e.g. the CSR.
    std::span<const std::string_view> sized_{};
    // Top-level keys left out entirely.
    std::span<const std::string_view> dropped_{};
};

// Every value outside the allow-list becomes the marker; arrays keep their
// first max_json_items items. `keep(output, value)` appends the stored form of
// a kept string value as a JSON string.
class json_excerpt final {
public:
    // nullopt when `body` is not JSON.
    [[nodiscard]] std::optional<std::string> document(std::string_view body, const json_excerpt_fields& fields,
        const auto& keep) {
        std::pmr::monotonic_buffer_resource pool;
        const auto parsed = ruvia::json_value::parse(body, {.resource_ = &pool});
        if (!parsed) {
            return std::nullopt;
        }
        std::string output;
        render(*parsed, fields, 0, output, keep);
        return output;
    }

    // Appends `value`, nested `depth` levels deep, to `output`.
    void render(const ruvia::json_value& value, const json_excerpt_fields& fields, int depth, std::string& output,
        const auto& keep) {
        std::string path;
        render(value, path, fields, false, depth, output, keep);
    }

    // Values replaced by the marker so far.
    [[nodiscard]] std::size_t redactions() const noexcept {
        return redactions_;
    }

    // Whether an array lost items to the item cap.
    [[nodiscard]] bool items_omitted() const noexcept {
        return items_omitted_;
    }

private:
    static constexpr int maximum_depth{16};

    void render(const ruvia::json_value& value, std::string& path, const json_excerpt_fields& fields, bool subtree,
        int depth, std::string& output, const auto& keep) {
        const auto indent = [&](int level) { output.append(static_cast<std::size_t>(level) * 2, ' '); };
        const auto kind = value.kind();
        if (kind == ruvia::json_value::kind_type::object || kind == ruvia::json_value::kind_type::array) {
            const bool descend = subtree || path.empty() || container_allowed(fields.allowed_, path);
            if (!descend || depth >= maximum_depth) {
                service::utils::append_json_string(output, redacted_marker);
                ++redactions_;
                return;
            }
            const bool whole = subtree || (!path.empty() && service::utils::contains(fields.allowed_, path));
            if (kind == ruvia::json_value::kind_type::object) {
                std::size_t written = 0;
                output.push_back('{');
                (void)value.for_each_field([&](std::string_view key, const ruvia::json_value& field) {
                    if (path.empty() && service::utils::contains(fields.dropped_, key)) {
                        return true;
                    }
                    output.append(written++ == 0 ? "\n" : ",\n");
                    indent(depth + 1);
                    service::utils::append_json_string(output, service::utils::sanitized_utf8(key, false));
                    output.append(": ");
                    const auto previous = path.size();
                    if (!path.empty()) {
                        path.push_back('.');
                    }
                    path.append(key);
                    render(field, path, fields, whole, depth + 1, output, keep);
                    path.resize(previous);
                    return true;
                });
                if (written > 0) {
                    output.push_back('\n');
                    indent(depth);
                }
                output.push_back('}');
                return;
            }
            std::size_t count = 0;
            output.push_back('[');
            const auto previous = path.size();
            path.append("[]");
            (void)value.for_each_element([&](const ruvia::json_value& element) {
                if (count++ >= max_json_items) {
                    return true;
                }
                output.append(count == 1 ? "\n" : ",\n");
                indent(depth + 1);
                render(element, path, fields, whole, depth + 1, output, keep);
                return true;
            });
            path.resize(previous);
            if (count > max_json_items) {
                items_omitted_ = true;
                output.append(",\n");
                indent(depth + 1);
                service::utils::append_json_string(output, "… " + std::to_string(count - max_json_items) + " more");
            }
            if (count > 0) {
                output.push_back('\n');
                indent(depth);
            }
            output.push_back(']');
            return;
        }
        if (kind == ruvia::json_value::kind_type::null) {
            output.append("null");
            return;
        }
        if (!subtree && !service::utils::contains(fields.allowed_, path)) {
            if (kind == ruvia::json_value::kind_type::string && service::utils::contains(fields.sized_, path)) {
                const auto decoded = value.get<ruvia::string>();
                const auto dot = path.rfind('.');
                std::string name = path.substr(dot == std::string::npos ? 0 : dot + 1);
                for (auto& ch : name) {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
                service::utils::append_json_string(output,
                    "[" + name + " " + std::to_string(decoded ? decoded->view().size() : 0) + " bytes]");
                return;
            }
            service::utils::append_json_string(output, redacted_marker);
            ++redactions_;
            return;
        }
        if (kind == ruvia::json_value::kind_type::string) {
            if (const auto decoded = value.get<ruvia::string>(); decoded) {
                keep(output, decoded->view());
            } else {
                service::utils::append_json_string(output, redacted_marker);
            }
            return;
        }
        output.append(service::utils::sanitized_utf8(value.view(), false));
    }

    [[nodiscard]] static bool container_allowed(std::span<const std::string_view> allowed, std::string_view path) {
        return std::any_of(allowed.begin(), allowed.end(), [path](std::string_view candidate) {
            return candidate == path ||
                   (candidate.size() > path.size() && candidate.starts_with(path) &&
                       (candidate[path.size()] == '.' || candidate[path.size()] == '['));
        });
    }

    std::size_t redactions_{};
    bool items_omitted_{};
};

}  // namespace service::attempt_trace
