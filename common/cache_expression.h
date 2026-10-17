#pragma once

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/route_match.h"

namespace flexedge::cache {

enum class expression_op { compare,
    logical_not,
    logical_and,
    logical_xor,
    logical_or,
    constant };
struct expression_instruction final {
    expression_op op_{expression_op::compare};
    std::string field_{};
    std::string header_{};
    std::string comparison_{};
    std::vector<std::string> values_{};
    bool lower_{false};
};

// A bounded parser shared by management validation and Worker compilation.
// Its result contains only values; regular expressions belong to the Worker.
class cache_expression_parser final {
public:
    explicit cache_expression_parser(std::string_view text)
        : text_(text) {}
    std::vector<expression_instruction> parse() {
        if (text_.empty() || text_.size() > 4096) {
            fail("表达式长度必须在1至4096之间");
        }
        logical_or(0);
        whitespace();
        if (position_ != text_.size()) {
            fail("表达式有未识别的内容");
        }
        return std::move(program_);
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw std::invalid_argument(std::string(message) + "（位置 " + std::to_string(position_ + 1) + "）");
    }
    void whitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\r' || text_[position_] == '\n')) {
            ++position_;
        }
    }
    bool symbol(char value) {
        whitespace();
        if (position_ == text_.size() || text_[position_] != value) {
            return false;
        }
        ++position_;
        return true;
    }
    void require(char value) {
        if (!symbol(value)) {
            fail("缺少分隔符");
        }
    }
    bool keyword(std::string_view word) {
        whitespace();
        if (text_.substr(position_, word.size()) != word) {
            return false;
        }
        const auto next = position_ + word.size();
        if (next < text_.size() && ((text_[next] >= 'a' && text_[next] <= 'z') || (text_[next] >= 'A' && text_[next] <= 'Z') || (text_[next] >= '0' && text_[next] <= '9') || text_[next] == '_' || text_[next] == '.')) {
            return false;
        }
        position_ = next;
        return true;
    }
    std::string identifier() {
        whitespace();
        const auto start = position_;
        while (position_ < text_.size()) {
            const auto byte = text_[position_];
            if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_' || byte == '.')) {
                break;
            }
            ++position_;
        }
        if (start == position_) {
            fail("需要字段或运算符");
        }
        return std::string(text_.substr(start, position_ - start));
    }
    std::string literal() {
        whitespace();
        std::size_t hashes{};
        const bool raw = position_ < text_.size() && text_[position_] == 'r';
        if (raw) {
            ++position_;
            while (position_ < text_.size() && text_[position_] == '#') {
                ++position_;
                if (++hashes > 8) {
                    fail("原始字符串分隔符过长");
                }
            }
        }
        if (position_ == text_.size() || text_[position_++] != '"') {
            fail("需要双引号字符串");
        }
        std::string value;
        while (position_ < text_.size()) {
            auto byte = text_[position_++];
            if (byte == '"' && (!raw || text_.substr(position_, hashes) == std::string(hashes, '#'))) {
                position_ += hashes;
                if (value.size() > 512 || !route_policy::valid_utf8(value)) {
                    fail("字符串无效或超过512字节");
                }
                return value;
            }
            if (!raw && byte == '\\') {
                if (position_ == text_.size()) {
                    fail("字符串转义不完整");
                }
                byte = text_[position_++];
                if (byte != '\\' && byte != '"') {
                    fail("不支持的字符串转义，请使用原始字符串");
                }
            }
            if (static_cast<unsigned char>(byte) < 32 || byte == 127) {
                fail("字符串不能包含控制字符");
            }
            value += byte;
        }
        fail("字符串未结束");
    }
    void emit(expression_instruction instruction) {
        if (program_.size() >= 256) {
            fail("表达式条件过多");
        }
        program_.push_back(std::move(instruction));
    }
    void field(expression_instruction& instruction) {
        instruction.lower_ = keyword("lower");
        if (instruction.lower_) {
            require('(');
        }
        instruction.field_ = identifier();
        constexpr std::array<std::string_view, 11> fields{"http.request.uri.path.extension", "http.request.uri.path", "raw.http.request.uri.path", "http.host", "http.cookie", "http.user_agent", "http.request.full_uri", "http.request.uri.query", "raw.http.request.uri.query", "http.referer", "http.request.method"};
        if (instruction.field_ == "http.request.headers") {
            require('[');
            instruction.header_ = literal();
            require(']');
            require('[');
            if (!symbol('0')) {
                fail("请求头仅支持索引0");
            }
            require(']');
            if (!route_policy::http_token(instruction.header_, 128) || std::ranges::any_of(instruction.header_, [](unsigned char c) { return c >= 'A' && c <= 'Z'; })) {
                fail("请求头名称必须使用小写");
            }
        } else if (std::ranges::find(fields, instruction.field_) == fields.end()) {
            fail("不支持此匹配字段");
        }
        if (instruction.lower_) {
            require(')');
        }
    }
    void primary(unsigned depth) {
        if (depth > 24) {
            fail("表达式嵌套过深");
        }
        if (symbol('(')) {
            logical_or(depth + 1);
            require(')');
            return;
        }
        if (keyword("not") || symbol('!')) {
            primary(depth + 1);
            emit({.op_ = expression_op::logical_not});
            return;
        }
        if (keyword("true")) {
            emit({.op_ = expression_op::constant, .values_ = {"true"}});
            return;
        }
        if (keyword("false")) {
            emit({.op_ = expression_op::constant});
            return;
        }
        expression_instruction instruction;
        if (keyword("starts_with")) {
            instruction.comparison_ = "starts_with";
        } else if (keyword("ends_with")) {
            instruction.comparison_ = "ends_with";
        }
        if (!instruction.comparison_.empty()) {
            require('(');
            field(instruction);
            require(',');
            instruction.values_.push_back(literal());
            require(')');
            emit(std::move(instruction));
            return;
        }
        field(instruction);
        if (symbol('=')) {
            require('=');
            instruction.comparison_ = "eq";
        } else if (symbol('!')) {
            require('=');
            instruction.comparison_ = "ne";
        } else if (symbol('~')) {
            instruction.comparison_ = "matches";
        } else {
            instruction.comparison_ = identifier();
            if (instruction.comparison_ == "strict") {
                if (!keyword("wildcard")) {
                    fail("strict后需要wildcard");
                }
                instruction.comparison_ = "strict wildcard";
            }
        }
        constexpr std::array<std::string_view, 7> comparisons{"eq", "ne", "contains", "wildcard", "strict wildcard", "matches", "in"};
        if (std::ranges::find(comparisons, instruction.comparison_) == comparisons.end()) {
            fail("不支持此运算符");
        }
        if (instruction.comparison_ == "in") {
            require('{');
            while (!symbol('}')) {
                if (instruction.values_.size() >= 100) {
                    fail("列表最多100项");
                }
                instruction.values_.push_back(literal());
            }
        } else {
            instruction.values_.push_back(literal());
        }
        if (instruction.comparison_ == "matches" && !route_policy::compile_pattern(instruction.values_.front())) {
            fail("正则表达式不受支持");
        }
        if (instruction.comparison_.ends_with("wildcard")) {
            bool escaped = false, star = false;
            for (const auto byte : instruction.values_.front()) {
                if (escaped) {
                    if (byte != '*' && byte != '\\') {
                        fail("通配符转义只支持星号和反斜线");
                    }
                    escaped = false;
                    star = false;
                } else if (byte == '\\') {
                    escaped = true;
                    star = false;
                } else if (byte == '*') {
                    if (star) {
                        fail("通配符不能包含连续星号");
                    }
                    star = true;
                } else {
                    star = false;
                }
            }
            if (escaped) {
                fail("通配符转义不完整");
            }
        }
        emit(std::move(instruction));
    }
    void logical_and(unsigned depth) {
        primary(depth);
        while (keyword("and") || (text_.substr(position_, 2) == "&&" && (position_ += 2))) {
            primary(depth);
            emit({.op_ = expression_op::logical_and});
        }
    }
    void logical_xor(unsigned depth) {
        logical_and(depth);
        while (keyword("xor")) {
            logical_and(depth);
            emit({.op_ = expression_op::logical_xor});
        }
    }
    void logical_or(unsigned depth) {
        logical_xor(depth);
        while (keyword("or") || (text_.substr(position_, 2) == "||" && (position_ += 2))) {
            logical_xor(depth);
            emit({.op_ = expression_op::logical_or});
        }
    }

    std::string_view text_;
    std::size_t position_{};
    std::vector<expression_instruction> program_{};
};

inline std::string validate_cache_expression(std::string_view text) {
    try {
        (void)cache_expression_parser(text).parse();
        return {};
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
}

}  // namespace flexedge::cache
