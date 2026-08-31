#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace flexedge::node {

enum class ChunkedWireStatus { kIncomplete, kComplete, kInvalid };

class ChunkedWireTracker final {
  public:
    [[nodiscard]] ChunkedWireStatus consume(std::string_view bytes) {
        if (state_ == State::kComplete) {
            return bytes.empty() ? ChunkedWireStatus::kComplete : ChunkedWireStatus::kInvalid;
        }
        if (state_ == State::kInvalid) {
            return ChunkedWireStatus::kInvalid;
        }
        for (const auto ch : bytes) {
            if (!consumeByte(ch)) {
                return fail();
            }
        }
        return state_ == State::kComplete ? ChunkedWireStatus::kComplete
                                          : ChunkedWireStatus::kIncomplete;
    }

  private:
    static constexpr std::size_t kMaxFramingBytes = 64 * 1024;
    static constexpr std::string_view kChunkDelimiter{"\r\n"};
    static constexpr std::string_view kTrailerEnd{"\r\n\r\n"};

    enum class State { kSizeLine, kBody, kDelimiter, kTrailers, kComplete, kInvalid };

    bool consumeByte(char ch) {
        switch (state_) {
        case State::kSizeLine:
            return consumeSizeLine(ch);
        case State::kBody:
            return consumeBody(ch);
        case State::kDelimiter:
            return consumeDelimiter(ch);
        case State::kTrailers:
            return consumeTrailers(ch);
        case State::kComplete:
        case State::kInvalid:
            return false;
        }
        return false;
    }

    bool consumeSizeLine(char ch) {
        if (!appendFraming(ch)) {
            return false;
        }
        if (!line_.ends_with(kChunkDelimiter)) {
            return true;
        }
        auto sizeText = std::string_view(line_).substr(0, line_.size() - kChunkDelimiter.size());
        if (const auto extension = sizeText.find(';'); extension != std::string_view::npos) {
            sizeText = sizeText.substr(0, extension);
        }
        std::size_t size{};
        const auto parsed =
            std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size, 16);
        if (sizeText.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != sizeText.data() + sizeText.size()) {
            return false;
        }
        line_.clear();
        if (size == 0) {
            state_ = State::kTrailers;
        } else {
            remaining_ = size;
            state_ = State::kBody;
        }
        return true;
    }

    bool consumeBody(char) {
        if (remaining_ == 0) {
            return false;
        }
        if (--remaining_ == 0) {
            delimiterOffset_ = 0;
            state_ = State::kDelimiter;
        }
        return true;
    }

    bool consumeDelimiter(char ch) {
        if (delimiterOffset_ >= kChunkDelimiter.size() || ch != kChunkDelimiter[delimiterOffset_]) {
            return false;
        }
        ++delimiterOffset_;
        if (delimiterOffset_ == kChunkDelimiter.size()) {
            state_ = State::kSizeLine;
        }
        return true;
    }

    bool consumeTrailers(char ch) {
        if (!appendFraming(ch)) {
            return false;
        }
        if (line_ == kChunkDelimiter || line_.ends_with(kTrailerEnd)) {
            state_ = State::kComplete;
        }
        return true;
    }

    bool appendFraming(char ch) {
        if (line_.size() >= kMaxFramingBytes) {
            return false;
        }
        line_.push_back(ch);
        return true;
    }

    ChunkedWireStatus fail() noexcept {
        state_ = State::kInvalid;
        return ChunkedWireStatus::kInvalid;
    }

    State state_{State::kSizeLine};
    std::string line_;
    std::size_t remaining_{};
    std::size_t delimiterOffset_{};
};

} // namespace flexedge::node
