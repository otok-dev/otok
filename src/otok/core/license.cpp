// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/license.hpp"

#include <algorithm>
#include <optional>

namespace otok {

namespace {

bool is_id_char(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == ':';
}

class license_parser
{
public:
    explicit license_parser(std::string_view text) : text_{text} { tokenize(); }

    result<license_expression> parse()
    {
        if (!error_.empty()) return fail(error_);
        if (tokens_.empty()) return fail("license expression is empty");
        auto e = or_expression();
        if (!e) return e;
        if (pos_ != tokens_.size()) return fail("unexpected '" + tokens_[pos_] + "' in license expression '" + std::string{text_} + "'");
        return e;
    }

private:
    void tokenize()
    {
        std::size_t i = 0;
        while (i < text_.size()) {
            char const c = text_[i];
            if (c == ' ') {
                ++i;
            } else if (c == '(' || c == ')') {
                tokens_.emplace_back(1, c);
                ++i;
            } else if (is_id_char(c)) {
                std::size_t const start = i;
                while (i < text_.size() && is_id_char(text_[i])) ++i;
                tokens_.emplace_back(text_.substr(start, i - start));
            } else {
                error_ = "license expression '" + std::string{text_} + "' contains '" + std::string(1, c) + "'";
                return;
            }
        }
    }

    bool accept(std::string_view token)
    {
        if (pos_ < tokens_.size() && tokens_[pos_] == token) {
            ++pos_;
            return true;
        }
        return false;
    }

    static bool is_operator(std::string_view t) noexcept
    {
        return t == "AND" || t == "OR" || t == "WITH" || t == "(" || t == ")";
    }

    result<license_expression> or_expression() { return binary(license_expression::kind::any_of, "OR", &license_parser::and_expression); }
    result<license_expression> and_expression() { return binary(license_expression::kind::all_of, "AND", &license_parser::primary); }

    result<license_expression> binary(license_expression::kind k, std::string_view op, result<license_expression> (license_parser::*next)())
    {
        auto first = (this->*next)();
        if (!first) return first;
        if (pos_ == tokens_.size() || tokens_[pos_] != op) return first;
        license_expression e{k, {}, {std::move(*first)}};
        while (accept(op)) {
            auto operand = (this->*next)();
            if (!operand) return operand;
            e.operands.push_back(std::move(*operand));
        }
        return e;
    }

    result<license_expression> primary()
    {
        if (accept("(")) {
            auto e = or_expression();
            if (!e) return e;
            if (!accept(")")) return fail("missing ')' in license expression '" + std::string{text_} + "'");
            return e;
        }
        if (pos_ == tokens_.size() || is_operator(tokens_[pos_])) {
            return fail("license expression '" + std::string{text_} + "' is missing a license id");
        }
        std::string id = tokens_[pos_++];
        if (accept("WITH")) {
            if (pos_ == tokens_.size() || is_operator(tokens_[pos_])) {
                return fail("license expression '" + std::string{text_} + "' is missing an exception after WITH");
            }
            id += " WITH " + tokens_[pos_++];
        }
        return license_expression{license_expression::kind::license, std::move(id), {}};
    }

    std::string_view text_;
    std::vector<std::string> tokens_;
    std::size_t pos_ = 0;
    std::string error_;
};

}

result<license_expression> parse_license_expression(std::string_view text)
{
    return license_parser{text}.parse();
}

bool license_satisfiable(license_expression const& e, std::function<bool(std::string_view id)> const& allowed)
{
    switch (e.type) {
    case license_expression::kind::license:
        return allowed(e.id);
    case license_expression::kind::all_of:
        return std::ranges::all_of(e.operands, [&](license_expression const& o) { return license_satisfiable(o, allowed); });
    case license_expression::kind::any_of:
        return std::ranges::any_of(e.operands, [&](license_expression const& o) { return license_satisfiable(o, allowed); });
    }
    return false;
}

}
