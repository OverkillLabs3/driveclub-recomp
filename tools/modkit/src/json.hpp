// SPDX-License-Identifier: GPL-2.0-or-later
// A small JSON reader and writer for mod manifests and patch files. Besides standard JSON it accepts comments
// (// and /* */) and a comma after the last item, because files written by hand are easier that way.
#pragma once

#include "common.hpp"

#include <cmath>
#include <utility>

namespace dcmod {

class Json {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : kind_(Kind::Bool), bool_(b) {}
    Json(int v) : kind_(Kind::Number), int_(v), num_(v), is_int_(true) {}
    Json(int64_t v) : kind_(Kind::Number), int_(v), num_(double(v)), is_int_(true) {}
    Json(uint32_t v) : Json(int64_t(v)) {}
    Json(size_t v) : Json(int64_t(v)) {}
    Json(double v) : kind_(Kind::Number), num_(v) {}
    Json(const char* s) : kind_(Kind::String), str_(s) {}
    Json(std::string s) : kind_(Kind::String), str_(std::move(s)) {}

    static Json Array() {
        Json j;
        j.kind_ = Kind::Array;
        return j;
    }
    static Json Object() {
        Json j;
        j.kind_ = Kind::Object;
        return j;
    }

    Kind kind() const { return kind_; }
    bool IsNull() const { return kind_ == Kind::Null; }
    bool IsBool() const { return kind_ == Kind::Bool; }
    bool IsNumber() const { return kind_ == Kind::Number; }
    bool IsString() const { return kind_ == Kind::String; }
    bool IsArray() const { return kind_ == Kind::Array; }
    bool IsObject() const { return kind_ == Kind::Object; }

    bool AsBool() const { return Need(Kind::Bool, "a true/false value"), bool_; }
    double AsNumber() const { return Need(Kind::Number, "a number"), num_; }
    int64_t AsInt() const {
        Need(Kind::Number, "a number");
        return is_int_ ? int_ : int64_t(std::llround(num_));
    }
    const std::string& AsString() const { return Need(Kind::String, "a string"), str_; }
    const std::vector<Json>& Items() const { return Need(Kind::Array, "a list"), items_; }
    const std::vector<Member>& Members() const { return Need(Kind::Object, "an object"), members_; }

    // Object access. Find returns null when the key is missing (or this is not an object).
    const Json* Find(std::string_view key) const {
        if (kind_ != Kind::Object) return nullptr;
        for (const Member& m : members_) {
            if (m.first == key) return &m.second;
        }
        return nullptr;
    }
    bool Has(std::string_view key) const { return Find(key) != nullptr; }
    std::string GetString(std::string_view key, const std::string& fallback = {}) const {
        const Json* j = Find(key);
        return j && j->IsString() ? j->str_ : fallback;
    }
    int64_t GetInt(std::string_view key, int64_t fallback = 0) const {
        const Json* j = Find(key);
        return j && j->IsNumber() ? j->AsInt() : fallback;
    }
    bool GetBool(std::string_view key, bool fallback = false) const {
        const Json* j = Find(key);
        return j && j->IsBool() ? j->bool_ : fallback;
    }

    Json& Set(const std::string& key, Json value) {
        if (kind_ == Kind::Null) kind_ = Kind::Object;
        Need(Kind::Object, "an object");
        for (Member& m : members_) {
            if (m.first == key) {
                m.second = std::move(value);
                return m.second;
            }
        }
        members_.emplace_back(key, std::move(value));
        return members_.back().second;
    }
    void Remove(std::string_view key) {
        for (size_t i = 0; i < members_.size(); ++i) {
            if (members_[i].first == key) {
                members_.erase(members_.begin() + std::ptrdiff_t(i));
                return;
            }
        }
    }
    Json& Push(Json value) {
        if (kind_ == Kind::Null) kind_ = Kind::Array;
        Need(Kind::Array, "a list");
        items_.push_back(std::move(value));
        return items_.back();
    }

    // Throws an Error that names the line and column of the first mistake.
    static Json Parse(std::string_view text);
    static Json ParseFile(const fs::path& path);

    std::string Dump(int indent = 2) const {
        std::string out;
        Write(out, indent, 0);
        out.push_back('\n');
        return out;
    }

private:
    void Need(Kind k, const char* what) const {
        if (kind_ != k) throw Error(std::string("expected ") + what + " in the JSON");
    }
    void Write(std::string& out, int indent, int depth) const;

    Kind kind_ = Kind::Null;
    bool bool_ = false;
    int64_t int_ = 0;
    double num_ = 0;
    bool is_int_ = false;
    std::string str_;
    std::vector<Json> items_;
    std::vector<Member> members_;
};

namespace json_detail {

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {
        if (t_.size() >= 3 && static_cast<unsigned char>(t_[0]) == 0xEF && static_cast<unsigned char>(t_[1]) == 0xBB &&
            static_cast<unsigned char>(t_[2]) == 0xBF) {
            p_ = 3;
        }
    }

    Json Run() {
        Json v = Value(0);
        Skip();
        if (p_ != t_.size()) Fail("unexpected text after the end of the value");
        return v;
    }

private:
    [[noreturn]] void Fail(const std::string& what) const {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < p_ && i < t_.size(); ++i) {
            if (t_[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        throw Error("JSON, line " + std::to_string(line) + " column " + std::to_string(col) + ": " + what);
    }

    void Skip() {
        for (;;) {
            while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\r' || t_[p_] == '\n')) ++p_;
            if (p_ + 1 < t_.size() && t_[p_] == '/' && t_[p_ + 1] == '/') {
                while (p_ < t_.size() && t_[p_] != '\n') ++p_;
            } else if (p_ + 1 < t_.size() && t_[p_] == '/' && t_[p_ + 1] == '*') {
                const size_t end = t_.find("*/", p_ + 2);
                if (end == std::string_view::npos) Fail("a comment is not closed");
                p_ = end + 2;
            } else {
                return;
            }
        }
    }

    bool Eat(char c) {
        Skip();
        if (p_ < t_.size() && t_[p_] == c) {
            ++p_;
            return true;
        }
        return false;
    }

    Json Value(int depth) {
        if (depth > 64) Fail("nested too deeply");
        Skip();
        if (p_ >= t_.size()) Fail("the text ends where a value should be");
        const char c = t_[p_];
        if (c == '{') return Obj(depth);
        if (c == '[') return Arr(depth);
        if (c == '"') return Json(Str());
        if (c == '-' || (c >= '0' && c <= '9')) return Num();
        if (t_.compare(p_, 4, "true") == 0) {
            p_ += 4;
            return Json(true);
        }
        if (t_.compare(p_, 5, "false") == 0) {
            p_ += 5;
            return Json(false);
        }
        if (t_.compare(p_, 4, "null") == 0) {
            p_ += 4;
            return Json();
        }
        Fail(std::string("unexpected character '") + c + "'");
    }

    Json Obj(int depth) {
        ++p_;
        Json o = Json::Object();
        for (;;) {
            Skip();
            if (Eat('}')) return o;
            Skip();
            if (p_ >= t_.size() || t_[p_] != '"') Fail("a name in quotes is expected");
            std::string key = Str();
            if (!Eat(':')) Fail("':' is expected after the name \"" + key + "\"");
            o.Set(key, Value(depth + 1));
            Skip();
            if (Eat(',')) continue;
            if (Eat('}')) return o;
            Fail("',' or '}' is expected");
        }
    }

    Json Arr(int depth) {
        ++p_;
        Json a = Json::Array();
        for (;;) {
            Skip();
            if (Eat(']')) return a;
            a.Push(Value(depth + 1));
            Skip();
            if (Eat(',')) continue;
            if (Eat(']')) return a;
            Fail("',' or ']' is expected");
        }
    }

    static void Utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    uint32_t Hex4() {
        if (p_ + 4 > t_.size()) Fail("a \\u escape is cut short");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else Fail("a \\u escape needs four hex digits");
        }
        return v;
    }

    std::string Str() {
        ++p_;
        std::string s;
        for (;;) {
            if (p_ >= t_.size()) Fail("a string is not closed");
            const char c = t_[p_++];
            if (c == '"') return s;
            if (static_cast<unsigned char>(c) < 0x20) Fail("a string holds a raw line break or control character");
            if (c != '\\') {
                s.push_back(c);
                continue;
            }
            if (p_ >= t_.size()) Fail("a string is not closed");
            const char e = t_[p_++];
            switch (e) {
                case '"': s.push_back('"'); break;
                case '\\': s.push_back('\\'); break;
                case '/': s.push_back('/'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = Hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && p_ + 1 < t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                        p_ += 2;
                        const uint32_t lo = Hex4();
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else Fail("a surrogate pair is broken");
                    }
                    Utf8(s, cp);
                    break;
                }
                default: Fail(std::string("unknown escape \\") + e);
            }
        }
    }

    Json Num() {
        const size_t start = p_;
        bool real = false;
        if (t_[p_] == '-') ++p_;
        while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
        if (p_ < t_.size() && t_[p_] == '.') {
            real = true;
            ++p_;
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
        }
        if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
            real = true;
            ++p_;
            if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
        }
        const std::string lit(t_.substr(start, p_ - start));
        if (lit.empty() || lit == "-") Fail("a number is expected");
        try {
            if (!real) return Json(int64_t(std::stoll(lit)));
            return Json(std::stod(lit));
        } catch (const std::exception&) {
            Fail("this number does not fit");
        }
    }

    std::string_view t_;
    size_t p_ = 0;
};

inline void Escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(char(c));
                }
        }
    }
    out.push_back('"');
}

}  // namespace json_detail

inline Json Json::Parse(std::string_view text) { return json_detail::Parser(text).Run(); }

inline Json Json::ParseFile(const fs::path& path) {
    try {
        return Parse(ReadText(path));
    } catch (const Error& e) {
        throw Error(path.string() + ": " + e.what());
    }
}

inline void Json::Write(std::string& out, int indent, int depth) const {
    const auto newline = [&](int d) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append(size_t(indent) * size_t(d), ' ');
    };
    switch (kind_) {
        case Kind::Null: out += "null"; break;
        case Kind::Bool: out += bool_ ? "true" : "false"; break;
        case Kind::Number:
            if (is_int_) {
                out += std::to_string(int_);
            } else {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.17g", num_);
                out += buf;
            }
            break;
        case Kind::String: json_detail::Escape(out, str_); break;
        case Kind::Array:
            if (items_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < items_.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                items_[i].Write(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back(']');
            break;
        case Kind::Object:
            if (members_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (size_t i = 0; i < members_.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                json_detail::Escape(out, members_[i].first);
                out += indent > 0 ? ": " : ":";
                members_[i].second.Write(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back('}');
            break;
    }
}

}  // namespace dcmod
