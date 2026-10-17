#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/rand.h>

#include "node/data/cache_range.h"

namespace flexedge::node {

// A request-local projection of one immutable representation. The same framing
// is used for cached files and origin responses, including several origin reads.
class cache_range_response final {
public:
    cache_range_response(std::string_view request, std::size_t total, bool partial_file, std::string content_type)
        : selection_(select_cache_ranges(request, total, partial_file)),
          total_(total),
          content_type_(std::move(content_type)) {
        if (selection_.multipart_) {
            std::array<unsigned char, 16> random{};
            if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
                throw std::runtime_error("range boundary generation failed");
            }
            constexpr std::string_view digits = "0123456789abcdef";
            boundary_ = "flexedge-";
            for (const auto byte : random) {
                boundary_ += digits[byte >> 4];
                boundary_ += digits[byte & 15];
            }
        }
        if (selection_.ignore_) {
            length_ = total;
        } else {
            for (std::size_t index = 0; index < selection_.ranges_.size(); ++index) {
                length_ += selection_.ranges_[index].length_;
                if (selection_.multipart_) {
                    length_ += prefix(index).size();
                }
            }
            if (selection_.multipart_) {
                length_ += ending().size();
            }
        }
    }

    [[nodiscard]] unsigned status() const noexcept {
        return selection_.ignore_ ? 200 : selection_.ranges_.empty() ? 416
                                                                     : 206;
    }
    [[nodiscard]] bool multipart() const noexcept {
        return selection_.multipart_;
    }
    [[nodiscard]] bool ignored() const noexcept {
        return selection_.ignore_;
    }
    [[nodiscard]] std::uint64_t length() const noexcept {
        return length_;
    }
    [[nodiscard]] const auto& ranges() const noexcept {
        return selection_.ranges_;
    }
    [[nodiscard]] std::string content_type() const {
        return multipart() ? "multipart/byteranges; boundary=" + boundary_ : content_type_;
    }
    [[nodiscard]] std::string content_range() const {
        if (status() == 416) {
            return "bytes */" + std::to_string(total_);
        }
        if (status() != 206 || multipart()) {
            return {};
        }
        const auto& range = selection_.ranges_.front();
        return "bytes " + std::to_string(range.offset_) + '-' + std::to_string(range.offset_ + range.length_ - 1) + '/' + std::to_string(total_);
    }
    [[nodiscard]] bool done() const noexcept {
        return ignored() ? consumed_ >= total_ : index_ == selection_.ranges_.size();
    }
    [[nodiscard]] std::size_t next_offset() const noexcept {
        return ignored() ? consumed_ : index_ < selection_.ranges_.size() ? (std::max)(consumed_, selection_.ranges_[index_].offset_)
                                                                          : total_;
    }
    std::string consume(std::string_view bytes, std::size_t offset) {
        if (ignored()) {
            consumed_ = offset + bytes.size();
            return std::string(bytes);
        }
        std::string output;
        const auto end = offset + bytes.size();
        while (index_ < selection_.ranges_.size()) {
            const auto& range = selection_.ranges_[index_];
            const auto first = (std::max)(offset, range.offset_);
            const auto last = (std::min)(end, range.offset_ + range.length_);
            if (first >= last) {
                break;
            }
            if (first == range.offset_ && multipart()) {
                output += prefix(index_);
            }
            output.append(bytes.substr(first - offset, last - first));
            consumed_ = last;
            if (last != range.offset_ + range.length_) {
                break;
            }
            ++index_;
            if (done() && multipart()) {
                output += ending();
            }
        }
        return output;
    }

private:
    std::string prefix(std::size_t index) const {
        const auto& range = selection_.ranges_[index];
        std::string result = index ? "\r\n--" : "--";
        result += boundary_ + "\r\n";
        if (!content_type_.empty()) {
            result += "Content-Type: " + content_type_ + "\r\n";
        }
        result += "Content-Range: bytes " + std::to_string(range.offset_) + '-' + std::to_string(range.offset_ + range.length_ - 1) + '/' + std::to_string(total_) + "\r\n\r\n";
        return result;
    }
    std::string ending() const {
        return "\r\n--" + boundary_ + "--\r\n";
    }

    cache_range_selection selection_;
    std::size_t total_{};
    std::string content_type_;
    std::string boundary_{};
    std::uint64_t length_{};
    std::size_t index_{};
    std::size_t consumed_{};
};

// Owned by one HTTP session. Origin exchanges only borrow this request state.
struct origin_range_request final {
    std::string_view client_range_{};
    std::optional<std::string_view> if_range_{};
    std::optional<cache_range_response> response_{};
    std::optional<std::size_t> total_{};
    std::string etag_{};
    bool has_validator_{false};
    bool head_started_{false};
    bool full_response_{false};
    bool passthrough_{false};
    std::size_t first_{};
    std::size_t last_{origin_range_block_bytes - 1};
};

}  // namespace flexedge::node
