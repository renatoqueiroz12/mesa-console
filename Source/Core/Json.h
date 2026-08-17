#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cmath>
#include <sstream>

namespace mesa { namespace json {

/** Arvore JSON minima, sem dependencias. Usada so fora da thread de audio. */
struct Value
{
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;

    bool        b   = false;
    double      num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    const Value* find (const std::string& key) const
    {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double      number (const std::string& k, double d = 0.0) const
    { auto* v = find (k); return (v && v->type == Number) ? v->num : d; }
    bool        boolean (const std::string& k, bool d = false) const
    { auto* v = find (k); return (v && v->type == Bool) ? v->b : d; }
    std::string string (const std::string& k, const std::string& d = {}) const
    { auto* v = find (k); return (v && v->type == String) ? v->str : d; }

    void set (const std::string& k, Value v) { obj.emplace_back (k, std::move (v)); }
};

inline Value num (double v)              { Value x; x.type = Value::Number; x.num = v; return x; }
inline Value boolean (bool v)            { Value x; x.type = Value::Bool;   x.b   = v; return x; }
inline Value text (const std::string& v) { Value x; x.type = Value::String; x.str = v; return x; }
inline Value object()                    { Value x; x.type = Value::Object; return x; }
inline Value array()                     { Value x; x.type = Value::Array;  return x; }

inline void escape (const std::string& s, std::string& out)
{
    for (char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
}

inline void write (const Value& v, std::string& out, int indent = 0)
{
    const std::string pad (size_t (indent * 2), ' ');
    const std::string pad2 (size_t ((indent + 1) * 2), ' ');

    switch (v.type)
    {
        case Value::Null:   out += "null"; break;
        case Value::Bool:   out += v.b ? "true" : "false"; break;
        case Value::Number:
        {
            std::ostringstream ss;
            if (std::fabs (v.num - std::round (v.num)) < 1e-9 && std::fabs (v.num) < 1e15)
                ss << (long long) std::llround (v.num);
            else
                ss.precision (6), ss << std::fixed << v.num;
            out += ss.str();
            break;
        }
        case Value::String: out += '"'; escape (v.str, out); out += '"'; break;
        case Value::Array:
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); ++i)
            {
                out += pad2; write (v.arr[i], out, indent + 1);
                out += (i + 1 < v.arr.size()) ? ",\n" : "\n";
            }
            out += pad + "]";
            break;
        case Value::Object:
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < v.obj.size(); ++i)
            {
                out += pad2 + '"'; escape (v.obj[i].first, out); out += "\": ";
                write (v.obj[i].second, out, indent + 1);
                out += (i + 1 < v.obj.size()) ? ",\n" : "\n";
            }
            out += pad + "}";
            break;
    }
}

inline std::string toString (const Value& v) { std::string s; write (v, s); return s; }

// ------------------------------------------------------------------ parser
inline void skipWs (const std::string& s, size_t& i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
}

inline bool parseValue (const std::string& s, size_t& i, Value& out);

inline bool parseString (const std::string& s, size_t& i, std::string& out)
{
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size() && s[i] != '"')
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            ++i;
            switch (s[i])
            {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                default:  out += s[i]; break;
            }
        }
        else out += s[i];
        ++i;
    }
    if (i >= s.size()) return false;
    ++i;
    return true;
}

inline bool parseValue (const std::string& s, size_t& i, Value& out)
{
    skipWs (s, i);
    if (i >= s.size()) return false;

    if (s[i] == '{')
    {
        out = object(); ++i; skipWs (s, i);
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (i < s.size())
        {
            skipWs (s, i);
            std::string key;
            if (! parseString (s, i, key)) return false;
            skipWs (s, i);
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            Value v;
            if (! parseValue (s, i, v)) return false;
            out.obj.emplace_back (key, std::move (v));
            skipWs (s, i);
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            return false;
        }
        return false;
    }

    if (s[i] == '[')
    {
        out = array(); ++i; skipWs (s, i);
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (i < s.size())
        {
            Value v;
            if (! parseValue (s, i, v)) return false;
            out.arr.push_back (std::move (v));
            skipWs (s, i);
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            return false;
        }
        return false;
    }

    if (s[i] == '"')
    {
        std::string str;
        if (! parseString (s, i, str)) return false;
        out = text (str); return true;
    }

    if (s.compare (i, 4, "true") == 0)  { out = boolean (true);  i += 4; return true; }
    if (s.compare (i, 5, "false") == 0) { out = boolean (false); i += 5; return true; }
    if (s.compare (i, 4, "null") == 0)  { out = Value{};         i += 4; return true; }

    char* end = nullptr;
    const double d = std::strtod (s.c_str() + i, &end);
    if (end == s.c_str() + i) return false;
    i = size_t (end - s.c_str());
    out = num (d);
    return true;
}

inline bool parse (const std::string& s, Value& out)
{
    size_t i = 0;
    return parseValue (s, i, out);
}

}} // namespace mesa::json
