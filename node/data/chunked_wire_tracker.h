#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace flexedge::node {

enum class chunked_wire_status { incomplete,
    complete,
    invalid };

class chunked_wire_tracker final {
public:
    [[nodiscard]] chunked_wire_status consume(std::string_view bytes) {
        if (state_ == state::complete) {
            return bytes.empty() ? chunked_wire_status::complete : chunked_wire_status::invalid;
        }
        if (state_ == state::invalid) {
            return chunked_wire_status::invalid;
        }
        for (const auto ch : bytes) {
            if (!consume_byte(ch)) {
                return fail();
            }
        }
        return state_ == state::complete ? chunked_wire_status::complete
                                         : chunked_wire_status::incomplete;
    }

private:
    static constexpr std::size_t max_framing_bytes = 64 * 1024;
    static constexpr std::string_view chunk_delimiter{"\r\n"};
    static constexpr std::string_view trailer_end{"\r\n\r\n"};

    enum class state { size_line,
        body,
        delimiter,
        trailers,
        complete,
        invalid };

    bool consume_byte(char ch) {
        switch (state_) {
            case state::size_line:
                return consume_size_line(ch);
            case state::body:
                return consume_body(ch);
            case state::delimiter:
                return consume_delimiter(ch);
            case state::trailers:
                return consume_trailers(ch);
            case state::complete:
            case state::invalid:
                return false;
        }
        return false;
    }

    bool consume_size_line(char ch) {
        if (!append_framing(ch)) {
            return false;
        }
        if (!line_.ends_with(chunk_delimiter)) {
            return true;
        }
        auto size_text = std::string_view(line_).substr(0, line_.size() - chunk_delimiter.size());
        if (const auto extension = size_text.find(';'); extension != std::string_view::npos) {
            size_text = size_text.substr(0, extension);
        }
        std::size_t size{};
        const auto parsed =
            std::from_chars(size_text.data(), size_text.data() + size_text.size(), size, 16);
        if (size_text.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != size_text.data() + size_text.size()) {
            return false;
        }
        line_.clear();
        if (size == 0) {
            state_ = state::trailers;
        } else {
            remaining_ = size;
            state_ = state::body;
        }
        return true;
    }

    bool consume_body(char) {
        if (remaining_ == 0) {
            return false;
        }
        if (--remaining_ == 0) {
            delimiter_offset_ = 0;
            state_ = state::delimiter;
        }
        return true;
    }

    bool consume_delimiter(char ch) {
        if (delimiter_offset_ >= chunk_delimiter.size() || ch != chunk_delimiter[delimiter_offset_]) {
            return false;
        }
        ++delimiter_offset_;
        if (delimiter_offset_ == chunk_delimiter.size()) {
            state_ = state::size_line;
        }
        return true;
    }

    bool consume_trailers(char ch) {
        if (!append_framing(ch)) {
            return false;
        }
        if (line_ == chunk_delimiter || line_.ends_with(trailer_end)) {
            state_ = state::complete;
        }
        return true;
    }

    bool append_framing(char ch) {
        if (line_.size() >= max_framing_bytes) {
            return false;
        }
        line_.push_back(ch);
        return true;
    }

    chunked_wire_status fail() noexcept {
        state_ = state::invalid;
        return chunked_wire_status::invalid;
    }

    state state_{state::size_line};
    std::string line_;
    std::size_t remaining_{};
    std::size_t delimiter_offset_{};
};

}  // namespace flexedge::node
