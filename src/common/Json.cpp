/*
 * Json.cpp — JSON 解析/序列化实现（递归下降）
 */
#include "msad/Json.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace msad {

/* ================= 序列化 ================= */

static void dumpString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

std::string Json::dump() const {
    std::string out;
    switch (type_) {
        case Type::Null:   out = "null"; break;
        case Type::Bool:   out = bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(number_)) return "null";
            char buf[64];
            if (std::floor(number_) == number_ && std::fabs(number_) < 1e15) {
                /* 整数形式输出，避免 "1.000000" */
                std::snprintf(buf, sizeof(buf), "%lld",
                              static_cast<long long>(number_));
            } else {
                std::snprintf(buf, sizeof(buf), "%.17g", number_);
            }
            out = buf;
            break;
        }
        case Type::String: dumpString(out, string_); break;
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& v : array()) {
                if (!first) out.push_back(',');
                first = false;
                out += v.dump();
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& kv : object()) {
                if (!first) out.push_back(',');
                first = false;
                dumpString(out, kv.first);
                out.push_back(':');
                out += kv.second.dump();
            }
            out.push_back('}');
            break;
        }
    }
    return out;
}

/* ================= 解析 ================= */

static void skipWs(Json::Parser& p) {
    while (p.pos < p.s.size() &&
           (p.s[p.pos] == ' ' || p.s[p.pos] == '\t' ||
            p.s[p.pos] == '\n' || p.s[p.pos] == '\r')) {
        ++p.pos;
    }
}

static bool parseString(Json::Parser& p, std::string& out) {
    if (p.pos >= p.s.size() || p.s[p.pos] != '"')
        return p.fail("expected '\"'");
    ++p.pos;
    out.clear();
    while (p.pos < p.s.size()) {
        char c = p.s[p.pos];
        if (c == '"') { ++p.pos; return true; }
        if (c == '\\') {
            if (p.pos + 1 >= p.s.size()) return p.fail("bad escape");
            char e = p.s[++p.pos];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'u': {
                    auto hex4 = [&](unsigned& code) {
                        if (p.pos + 4 >= p.s.size()) return false;
                        code = 0;
                        for (size_t i=1;i<=4;++i) {
                            const char h=p.s[p.pos+i]; code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h-'0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h-'a'+10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h-'A'+10);
                            else return false;
                        }
                        p.pos += 4; return true;
                    };
                    unsigned code;
                    if (!hex4(code)) return p.fail("bad unicode escape");
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (p.pos+2 >= p.s.size() || p.s[p.pos+1] != '\\' || p.s[p.pos+2] != 'u')
                            return p.fail("missing low surrogate");
                        p.pos += 2;
                        unsigned low;
                        if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return p.fail("invalid low surrogate");
                        code = 0x10000 + ((code-0xD800)<<10) + low-0xDC00;
                    } else if (code >= 0xDC00 && code <= 0xDFFF) return p.fail("unexpected low surrogate");
                    if (code < 0x80) out.push_back(static_cast<char>(code));
                    else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code>>6)));
                        out.push_back(static_cast<char>(0x80 | (code&0x3F)));
                    } else if (code < 0x10000) {
                        out.push_back(static_cast<char>(0xE0 | (code>>12)));
                        out.push_back(static_cast<char>(0x80 | ((code>>6)&0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code&0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xF0 | (code>>18)));
                        out.push_back(static_cast<char>(0x80 | ((code>>12)&0x3F)));
                        out.push_back(static_cast<char>(0x80 | ((code>>6)&0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code&0x3F)));
                    }
                    break;
                }
                default: return p.fail("unknown escape");
            }
            ++p.pos;
        } else {
            if (static_cast<unsigned char>(c) < 0x20) return p.fail("unescaped control character");
            out.push_back(c);
            ++p.pos;
        }
    }
    return p.fail("unterminated string");
}

static bool parseNumber(Json::Parser& p, double& out) {
    size_t start = p.pos;
    auto digit = [&] { return p.pos < p.s.size() && p.s[p.pos] >= '0' && p.s[p.pos] <= '9'; };
    if (p.pos < p.s.size() && p.s[p.pos] == '-') ++p.pos;
    if (!digit()) return p.fail("expected number");
    if (p.s[p.pos] == '0') ++p.pos;
    else while (digit()) ++p.pos;
    if (p.pos < p.s.size() && p.s[p.pos] == '.') {
        ++p.pos;
        if (!digit()) return p.fail("expected fractional digits");
        while (digit()) ++p.pos;
    }
    if (p.pos < p.s.size() && (p.s[p.pos] == 'e' || p.s[p.pos] == 'E')) {
        ++p.pos;
        if (p.pos < p.s.size() && (p.s[p.pos] == '+' || p.s[p.pos] == '-')) ++p.pos;
        if (!digit()) return p.fail("expected exponent digits");
        while (digit()) ++p.pos;
    }
    const std::string token = p.s.substr(start, p.pos - start);
    char* end = nullptr;
    out = std::strtod(token.c_str(), &end);
    if (*end != 0 || !std::isfinite(out)) return p.fail("invalid or overflowing number");
    return true;
}

bool Json::parseValue(Parser& p, Json& out) {
    if (p.depth >= 128) return p.fail("JSON nesting limit exceeded");
    struct Depth { size_t& value; explicit Depth(size_t& v):value(v){++value;} ~Depth(){--value;} } depth(p.depth);
    skipWs(p);
    if (p.pos >= p.s.size()) return p.fail("unexpected end");

    char c = p.s[p.pos];
    switch (c) {
        case '{': {
            ++p.pos;
            Json obj(Type::Object);
            skipWs(p);
            if (p.pos < p.s.size() && p.s[p.pos] == '}') { ++p.pos; out = obj; return true; }
            for (;;) {
                skipWs(p);
                std::string key;
                if (!parseString(p, key)) return false;
                skipWs(p);
                if (p.pos >= p.s.size() || p.s[p.pos] != ':') return p.fail("expected ':'");
                ++p.pos;
                Json val;
                if (!parseValue(p, val)) return false;
                obj[key] = val;
                skipWs(p);
                if (p.pos >= p.s.size()) return p.fail("unterminated object");
                if (p.s[p.pos] == ',') { ++p.pos; continue; }
                if (p.s[p.pos] == '}') { ++p.pos; out = obj; return true; }
                return p.fail("expected ',' or '}'");
            }
        }
        case '[': {
            ++p.pos;
            Json arr(Type::Array);
            skipWs(p);
            if (p.pos < p.s.size() && p.s[p.pos] == ']') { ++p.pos; out = arr; return true; }
            for (;;) {
                Json val;
                if (!parseValue(p, val)) return false;
                arr.array().push_back(val);
                skipWs(p);
                if (p.pos >= p.s.size()) return p.fail("unterminated array");
                if (p.s[p.pos] == ',') { ++p.pos; continue; }
                if (p.s[p.pos] == ']') { ++p.pos; out = arr; return true; }
                return p.fail("expected ',' or ']'");
            }
        }
        case '"': {
            std::string s;
            if (!parseString(p, s)) return false;
            out = Json(s);
            return true;
        }
        case 't': {
            if (p.s.compare(p.pos, 4, "true") == 0) { p.pos += 4; out = Json(true); return true; }
            return p.fail("bad literal");
        }
        case 'f': {
            if (p.s.compare(p.pos, 5, "false") == 0) { p.pos += 5; out = Json(false); return true; }
            return p.fail("bad literal");
        }
        case 'n': {
            if (p.s.compare(p.pos, 4, "null") == 0) { p.pos += 4; out = Json(); return true; }
            return p.fail("bad literal");
        }
        default: {
            double num = 0.0;
            if (!parseNumber(p, num)) return false;
            out = Json(num);
            return true;
        }
    }
}

bool Json::parse(const std::string& text, Json& out, std::string& err) {
    err.clear();
    Parser p{text, 0, ""};
    if (!parseValue(p, out)) {
        err = p.err;
        return false;
    }
    skipWs(p);
    if (p.pos != text.size()) {
        err = "trailing characters after value";
        return false;
    }
    return true;
}

} // namespace msad
