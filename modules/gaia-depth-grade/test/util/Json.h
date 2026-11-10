// Minimal JSON reader for the golden stage.json (test tooling). Numbers are parsed with
// strtod, which round-trips the repr floats Python's json module writes.
#pragma once

#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gdg::test {

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    bool is_null() const { return kind == Null; }
    const Json& operator[](const std::string& k) const {
        auto it = obj.find(k);
        if (kind != Object || it == obj.end()) throw std::runtime_error("Json: no key '" + k + "'");
        return it->second;
    }
    const Json& operator[](size_t i) const {
        if (kind != Array || i >= arr.size()) throw std::runtime_error("Json: index out of range");
        return arr[i];
    }
    bool has(const std::string& k) const { return kind == Object && obj.count(k) != 0; }

    static Json parse(const std::string& text) {
        size_t p = 0;
        Json j = value(text, p);
        ws(text, p);
        if (p != text.size()) throw std::runtime_error("Json: trailing data");
        return j;
    }
    static Json parse_file(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Json: cannot open " + path);
        std::stringstream ss;
        ss << in.rdbuf();
        return parse(ss.str());
    }

private:
    static void ws(const std::string& t, size_t& p) {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p;
    }
    static void expect(const std::string& t, size_t& p, const char* lit) {
        for (const char* c = lit; *c; ++c, ++p)
            if (p >= t.size() || t[p] != *c) throw std::runtime_error("Json: syntax error");
    }
    static std::string string_lit(const std::string& t, size_t& p) {
        expect(t, p, "\"");
        std::string out;
        while (p < t.size() && t[p] != '"') {
            char c = t[p++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= t.size()) break;
            c = t[p++];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                const unsigned cp = static_cast<unsigned>(std::stoul(t.substr(p, 4), nullptr, 16));
                p += 4;
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out += c;
            }
        }
        expect(t, p, "\"");
        return out;
    }
    static Json value(const std::string& t, size_t& p) {
        ws(t, p);
        Json j;
        if (p >= t.size()) throw std::runtime_error("Json: unexpected end");
        const char c = t[p];
        if (c == '{') {
            j.kind = Object;
            ++p;
            ws(t, p);
            if (t[p] == '}') {
                ++p;
                return j;
            }
            for (;;) {
                ws(t, p);
                std::string k = string_lit(t, p);
                ws(t, p);
                expect(t, p, ":");
                j.obj[k] = value(t, p);
                ws(t, p);
                if (t[p] == ',') {
                    ++p;
                    continue;
                }
                expect(t, p, "}");
                return j;
            }
        }
        if (c == '[') {
            j.kind = Array;
            ++p;
            ws(t, p);
            if (t[p] == ']') {
                ++p;
                return j;
            }
            for (;;) {
                j.arr.push_back(value(t, p));
                ws(t, p);
                if (t[p] == ',') {
                    ++p;
                    continue;
                }
                expect(t, p, "]");
                return j;
            }
        }
        if (c == '"') {
            j.kind = String;
            j.str = string_lit(t, p);
            return j;
        }
        if (t.compare(p, 4, "null") == 0) {
            p += 4;
            return j;
        }
        if (t.compare(p, 4, "true") == 0) {
            p += 4;
            j.kind = Bool;
            j.b = true;
            return j;
        }
        if (t.compare(p, 5, "false") == 0) {
            p += 5;
            j.kind = Bool;
            return j;
        }
        // NaN / Infinity as Python's json writes them
        if (t.compare(p, 3, "NaN") == 0) {
            p += 3;
            j.kind = Number;
            j.num = std::numeric_limits<double>::quiet_NaN();
            return j;
        }
        char* end = nullptr;
        j.num = std::strtod(t.c_str() + p, &end);
        if (end == t.c_str() + p) throw std::runtime_error("Json: bad value");
        p = static_cast<size_t>(end - t.c_str());
        j.kind = Number;
        return j;
    }
};

}  // namespace gdg::test
