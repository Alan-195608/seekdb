#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mini {

struct SqlError : std::runtime_error {
    std::string code;
    SqlError(std::string code, const std::string& message)
        : std::runtime_error(message), code(std::move(code)) {}
};

[[noreturn]] inline void syntax(const std::string& message) { throw SqlError("SYNTAX", message); }
[[noreturn]] inline void invalid(const std::string& message) { throw SqlError("INVALID_VALUE", message); }

enum class Kind : uint8_t { Integer, String, Vector };
using Vector = std::vector<float>;
using Value = std::variant<int64_t, std::string, Vector>;

struct Column {
    std::string name;
    Kind kind;
    uint32_t width = 0;
};

inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

inline std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}

inline size_t utf8_length(std::string_view input) {
    size_t count = 0;
    for (size_t i = 0; i < input.size(); ++count) {
        const unsigned char lead = input[i++];
        if (lead < 0x80) continue;
        unsigned extra;
        uint32_t cp;
        if (lead >= 0xc2 && lead <= 0xdf) { extra = 1; cp = lead & 31; }
        else if (lead >= 0xe0 && lead <= 0xef) { extra = 2; cp = lead & 15; }
        else if (lead >= 0xf0 && lead <= 0xf4) { extra = 3; cp = lead & 7; }
        else invalid("invalid UTF-8");
        if (extra > input.size() - i) invalid("truncated UTF-8");
        for (unsigned j = 0; j < extra; ++j) {
            const unsigned char next = input[i++];
            if ((next & 0xc0) != 0x80) invalid("invalid UTF-8 continuation");
            cp = (cp << 6) | (next & 63);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) invalid("invalid UTF-8 code point");
    }
    return count;
}

class Parser {
    enum class Type { End, Word, Quoted, String, Number, Symbol };
    std::string_view input_;
    size_t pos_ = 0;
    Type type_ = Type::End;
    std::string_view raw_;
    std::string quoted_;

    void skip() {
        while (pos_ < input_.size()) {
            if (space(input_[pos_])) { ++pos_; continue; }
            auto rest = input_.substr(pos_);
            if (rest.starts_with("--") || rest.starts_with("#")) {
                const auto end = input_.find('\n', pos_);
                pos_ = end == input_.npos ? input_.size() : end + 1;
            } else if (rest.starts_with("/*")) {
                const auto end = input_.find("*/", pos_ + 2);
                if (end == input_.npos) syntax("unterminated comment");
                pos_ = end + 2;
            } else break;
        }
    }

    void next() {
        skip();
        if (pos_ == input_.size()) { type_ = Type::End; raw_ = {}; return; }
        const size_t start = pos_;
        const char c = input_[pos_++];
        if (c == '\'' || c == '`') {
            type_ = c == '\'' ? Type::String : Type::Quoted;
            quoted_.clear();
            while (pos_ < input_.size()) {
                const char ch = input_[pos_++];
                if (ch == c) {
                    if (pos_ == input_.size() || input_[pos_] != c) return;
                    ++pos_;
                }
                quoted_.push_back(ch);
            }
            syntax("unterminated quoted token");
        } else if (alpha(c)) {
            type_ = Type::Word;
            while (pos_ < input_.size() && (alpha(input_[pos_]) || digit(input_[pos_]))) ++pos_;
        } else if (digit(c) || c == '+' || c == '-' || c == '.') {
            type_ = Type::Number;
            while (pos_ < input_.size()) {
                const char ch = input_[pos_];
                if (!digit(ch) && ch != '.' && ch != 'e' && ch != 'E' && ch != '+' && ch != '-') break;
                ++pos_;
            }
        } else {
            type_ = Type::Symbol;
            if ((c == '<' || c == '>') && pos_ < input_.size() && input_[pos_] == '=') ++pos_;
        }
        raw_ = input_.substr(start, pos_ - start);
    }

    static bool equals_word(std::string_view token, std::string_view word) {
        if (token.size() != word.size()) return false;
        for (size_t i = 0; i < word.size(); ++i) {
            auto c = token[i];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != word[i]) return false;
        }
        return true;
    }

    std::string_view number() {
        if (type_ != Type::Number) invalid("expected numeric literal");
        auto s = raw_;
        next();
        if (s.starts_with('+')) s.remove_prefix(1);
        return s;
    }

    float scalar() {
        const auto s = number();
        double parsed;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), parsed);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(parsed)) {
            invalid("invalid vector element");
        }
        const float value = static_cast<float>(parsed);
        if (!std::isfinite(value)) invalid("vector element overflows float32");
        return value;
    }

    Vector array(size_t dimension) {
        Vector result;
        need("[");
        if (!take("]")) {
            do {
                if (result.size() == dimension) throw SqlError("DIMENSION_MISMATCH", "vector dimension mismatch");
                result.push_back(scalar());
            } while (take(","));
            need("]");
        }
        if (result.size() != dimension) throw SqlError("DIMENSION_MISMATCH", "vector dimension mismatch");
        return result;
    }

public:
    explicit Parser(std::string_view input) : input_(input) { next(); }

    bool at(std::string_view token) const {
        return (type_ == Type::Word && equals_word(raw_, token)) ||
               (type_ == Type::Symbol && raw_ == token);
    }
    bool take(std::string_view token) {
        if (!at(token)) return false;
        next();
        return true;
    }
    void need(std::string_view token) {
        if (!take(token)) syntax("expected " + std::string(token));
    }
    bool empty() const { return type_ == Type::End; }
    void finish() {
        take(";");
        if (!empty()) syntax("unexpected tokens after statement");
    }
    std::string identifier() {
        if (type_ != Type::Word && type_ != Type::Quoted) syntax("expected identifier");
        auto name = type_ == Type::Quoted ? quoted_ : std::string(raw_);
        if (name.empty()) syntax("empty identifier");
        next();
        return lower(std::move(name));
    }
    int64_t integer() {
        const auto s = number();
        int64_t value;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), value);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) invalid("invalid BIGINT");
        return value;
    }
    void skip_vector_literal() {
        if (type_ == Type::String) { next(); return; }
        need("[");
        while (!take("]")) {
            if (empty()) syntax("unterminated vector");
            next();
        }
    }
    Vector vector(size_t dimension) {
        if (type_ != Type::String) return array(dimension);
        Parser parser(quoted_);
        auto value = parser.array(dimension);
        if (!parser.empty()) invalid("trailing vector data");
        next();
        return value;
    }
    Value value(const Column& column) {
        if (column.kind == Kind::Integer) return integer();
        if (column.kind == Kind::Vector) return vector(column.width);
        if (type_ != Type::String) invalid("expected string literal");
        auto value = std::move(quoted_);
        next();
        if (utf8_length(value) > column.width) invalid("VARCHAR length exceeded");
        return value;
    }
};

} // namespace mini
