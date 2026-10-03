// ============================================================
//  json.hpp — 极简 JSON 解析 / 序列化（UTF-8 透传）
//  无外部依赖，支持完整 JSON 语法（含 \uXXXX 代理对）
// ============================================================
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::shared_ptr<Array> arr;
    std::shared_ptr<Object> obj;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : type(Type::Bool), b(v) {}
    Value(double v) : type(Type::Number), num(v) {}
    Value(int v) : type(Type::Number), num((double)v) {}
    Value(unsigned v) : type(Type::Number), num((double)v) {}
    Value(long long v) : type(Type::Number), num((double)v) {}
    Value(const char* v) : type(Type::String), str(v ? v : "") {}
    Value(std::string v) : type(Type::String), str(std::move(v)) {}

    static Value mkArray() { Value v; v.type = Type::Array; v.arr = std::make_shared<Array>(); return v; }
    static Value mkObject() { Value v; v.type = Type::Object; v.obj = std::make_shared<Object>(); return v; }

    bool isNull() const { return type == Type::Null; }
    bool has(const std::string& k) const { return type == Type::Object && obj && obj->count(k) > 0; }
    const Value& get(const std::string& k) const {
        static const Value nullv;
        if (type != Type::Object || !obj) return nullv;
        auto it = obj->find(k);
        return it == obj->end() ? nullv : it->second;
    }
    const Value& operator[](const std::string& k) const { return get(k); }
    const Value& at(size_t i) const {
        static const Value nullv;
        if (type != Type::Array || !arr || i >= arr->size()) return nullv;
        return (*arr)[i];
    }
    size_t size() const {
        if (type == Type::Array && arr) return arr->size();
        if (type == Type::Object && obj) return obj->size();
        return 0;
    }
    std::string getStr(const std::string& def = "") const { return type == Type::String ? str : def; }
    double getNum(double def = 0.0) const { return type == Type::Number ? num : def; }
    long long getInt(long long def = 0) const { return type == Type::Number ? (long long)num : def; }
    bool getBool(bool def = false) const { return type == Type::Bool ? b : def; }
};

namespace detail {

inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error("json: invalid hex escape");
}

inline void encodeUtf8(unsigned cp, std::string& out) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Value parse() {
        skipWs();
        Value v = parseValue();
        skipWs();
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skipWs() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') i_++;
            else break;
        }
    }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    char next() {
        if (i_ >= s_.size()) throw std::runtime_error("json: unexpected end");
        return s_[i_++];
    }
    void expect(char c) {
        skipWs();
        if (peek() != c) throw std::runtime_error(std::string("json: expected '") + c + "'");
        i_++;
    }

    Value parseValue() {
        skipWs();
        char c = peek();
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': { Value v; v.type = Type::String; v.str = parseString(); return v; }
            case 't': eatLiteral("true"); return Value(true);
            case 'f': eatLiteral("false"); return Value(false);
            case 'n': eatLiteral("null"); return Value();
            default: return parseNumber();
        }
    }

    void eatLiteral(const char* lit) {
        for (const char* p = lit; *p; p++) {
            if (next() != *p) throw std::runtime_error("json: bad literal");
        }
    }

    Value parseNumber() {
        size_t start = i_;
        if (peek() == '-') i_++;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' ||
               s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) i_++;
        if (i_ == start) throw std::runtime_error("json: bad number");
        std::string tok = s_.substr(start, i_ - start);
        char* endp = nullptr;
        double d = strtod(tok.c_str(), &endp);
        Value v;
        v.type = Type::Number;
        v.num = d;
        return v;
    }

    std::string parseString() {
        next(); // 引号
        std::string out;
        while (true) {
            if (i_ >= s_.size()) throw std::runtime_error("json: unterminated string");
            char c = s_[i_++];
            if (c == '"') break;
            if (c == '\\') {
                char e = next();
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp = 0;
                        for (int k = 0; k < 4; k++) cp = cp * 16 + (unsigned)hexVal(next());
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                                i_ += 2;
                                unsigned lo = 0;
                                for (int k = 0; k < 4; k++) lo = lo * 16 + (unsigned)hexVal(next());
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                        }
                        encodeUtf8(cp, out);
                        break;
                    }
                    default: throw std::runtime_error("json: bad escape");
                }
            } else if ((unsigned char)c < 0x20) {
                throw std::runtime_error("json: control char in string");
            } else {
                out += c;
            }
        }
        return out;
    }

    Value parseObject() {
        next(); // {
        Value v = Value::mkObject();
        skipWs();
        if (peek() == '}') { i_++; return v; }
        while (true) {
            skipWs();
            if (peek() != '"') throw std::runtime_error("json: expected key");
            std::string key = parseString();
            expect(':');
            Value val = parseValue();
            (*v.obj)[key] = val;
            skipWs();
            char c = next();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("json: expected , or }");
        }
        return v;
    }

    Value parseArray() {
        next(); // [
        Value v = Value::mkArray();
        skipWs();
        if (peek() == ']') { i_++; return v; }
        while (true) {
            v.arr->push_back(parseValue());
            skipWs();
            char c = next();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("json: expected , or ]");
        }
        return v;
    }
};

inline std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += (char)c;
                }
        }
    }
    return o + "\"";
}

} // namespace detail

inline Value parse(const std::string& s) {
    detail::Parser p(s);
    return p.parse();
}

inline std::string dump(const Value& v) {
    using detail::quote;
    switch (v.type) {
        case Type::Null: return "null";
        case Type::Bool: return v.b ? "true" : "false";
        case Type::Number: {
            if (std::isnan(v.num) || std::isinf(v.num)) return "null";
            if (v.num == std::floor(v.num) && std::fabs(v.num) < 9e15) {
                char buf[32];
                snprintf(buf, sizeof buf, "%lld", (long long)v.num);
                return buf;
            }
            char buf[48];
            snprintf(buf, sizeof buf, "%.12g", v.num);
            return buf;
        }
        case Type::String: return quote(v.str);
        case Type::Array: {
            std::string o = "[";
            bool first = true;
            for (const auto& e : *v.arr) {
                if (!first) o += ",";
                first = false;
                o += dump(e);
            }
            o += "]";
            return o;
        }
        case Type::Object: {
            std::string o = "{";
            bool first = true;
            for (const auto& kv : *v.obj) {
                if (!first) o += ",";
                first = false;
                o += quote(kv.first);
                o += ":";
                o += dump(kv.second);
            }
            o += "}";
            return o;
        }
    }
    return "null";
}

} // namespace json
