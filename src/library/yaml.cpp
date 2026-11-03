/******************************************************************************
 * yaml.cpp
 * rfx library - a small document tree written as block-style YAML
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "yaml.h"

#include <QByteArray>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <cstdio>

namespace rfx {

namespace {

template<typename Real>
Real parseReal(const char *text);

template<>
double parseReal<double>(const char *text)
{
    return std::strtod(text, nullptr);
}

template<>
float parseReal<float>(const char *text)
{
    return std::strtof(text, nullptr);
}

// The shortest decimal digits that read back as `v`, and the exponent of the
// first of them (v = d.ddd * 10^exponent). `v` is positive and finite.
template<typename Real>
void shortestDigits(Real v, std::string &digits, int &exponent)
{
    const int maxDigits = std::numeric_limits<Real>::max_digits10;
    char buf[64];
    for (int precision = 0; precision < maxDigits; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*e", precision, static_cast<double>(v));

        if (parseReal<Real>(buf) == v) {
            break;
        }
    }
    digits.clear();
    const char *p = buf;
    for (; *p != 'e'; ++p) {
        if (*p != '.') {
            digits.push_back(*p);
        }
    }
    exponent = std::atoi(p + 1);
}

// A real number as JSON writes it (the shortest digits that read back the
// same, laid out as the ryu library does): "75000000.0", "0.001", "1.5e-7".
template<typename Real>
std::string realText(Real v)
{
    if (v == 0) {
        return std::signbit(v) ? "-0.0" : "0.0";
    }
    std::string digits;
    int exp10 = 0;
    shortestDigits(std::fabs(v), digits, exp10);
    const int length = static_cast<int>(digits.size());
    // The value is 0.<digits> * 10^kk.
    const int kk = exp10 + 1;
    std::string out = v < 0 ? "-" : "";
    if (kk >= length && kk <= 16) {
        out += digits + std::string(static_cast<size_t>(kk - length), '0') + ".0";
    } else if (kk > 0 && kk <= 16) {
        out += digits.substr(0, static_cast<size_t>(kk)) + "." + digits.substr(static_cast<size_t>(kk));
    } else if (kk > -5 && kk <= 0) {
        out += "0." + std::string(static_cast<size_t>(-kk), '0') + digits;
    } else {
        out += digits.substr(0, 1);
        if (length > 1) {
            out += "." + digits.substr(1);
        }
        out += "e" + std::to_string(kk - 1);
    }
    return out;
}

std::vector<std::string> splitLines(const std::string &s)
{
    // As str::lines: split at \n, drop a \r before it, no empty last line.
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        const bool last = end == std::string::npos;
        if (last) {
            end = s.size();
        }
        size_t stop = end;
        if (!last && stop > start && s[stop - 1] == '\r') {
            --stop;
        }
        out.push_back(s.substr(start, stop - start));
        start = end + 1;
    }
    return out;
}

} // namespace

std::string formatReal(double v)
{
    return realText<double>(v);
}

std::string formatReal(float v)
{
    return realText<float>(v);
}

std::string jsonQuote(const std::string &s)
{
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(ch);
            }
        }
    }
    out += '"';
    return out;
}

Node Node::boolean(bool v)
{
    Node n;
    n.m_kind = Kind::Bool;
    n.m_bool = v;
    return n;
}

Node Node::integer(int64_t v)
{
    Node n;
    n.m_kind = Kind::Integer;
    n.m_negative = v < 0;
    n.m_magnitude = v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
    return n;
}

Node Node::unsignedInteger(uint64_t v)
{
    Node n;
    n.m_kind = Kind::Integer;
    n.m_magnitude = v;
    return n;
}

Node Node::real(double v)
{
    Node n;
    n.m_kind = Kind::Float;
    n.m_real = v;
    return n;
}

Node Node::string(const std::string &v)
{
    Node n;
    n.m_kind = Kind::String;
    n.m_text = v;
    return n;
}

Node Node::array()
{
    Node n;
    n.m_kind = Kind::Array;
    return n;
}

Node Node::object()
{
    Node n;
    n.m_kind = Kind::Object;
    return n;
}

Node &Node::push(const Node &value)
{
    m_items.push_back(value);
    return *this;
}

Node &Node::set(const std::string &key, const Node &value)
{
    const auto it = std::lower_bound(m_keys.begin(), m_keys.end(), key);
    const size_t at = static_cast<size_t>(it - m_keys.begin());
    if (it != m_keys.end() && *it == key) {
        m_items[at] = value;
    } else {
        m_keys.insert(it, key);
        m_items.insert(m_items.begin() + static_cast<std::ptrdiff_t>(at), value);
    }
    return *this;
}

std::string Node::scalar() const
{
    switch (m_kind) {
    case Kind::Null: return "null";
    case Kind::Bool: return m_bool ? "true" : "false";
    case Kind::Integer: return (m_negative ? "-" : "") + std::to_string(m_magnitude);
    case Kind::Float: return realText(m_real);
    case Kind::String: return jsonQuote(m_text);
    case Kind::Array: {
        std::string out = "[";
        for (size_t i = 0; i < m_items.size(); ++i) {
            out += (i ? ", " : "") + m_items[i].scalar();
        }
        return out + "]";
    }
    case Kind::Object: break;
    }
    return "{}";
}

void Node::writeYaml(size_t indent, std::string &out) const
{
    const std::string pad(indent, ' ');
    if (m_kind == Kind::Object) {
        for (size_t i = 0; i < m_items.size(); ++i) {
            const std::string &key = m_keys[i];
            const Node &val = m_items[i];
            const bool nested = val.m_kind == Kind::Object && !val.isEmpty();
            const bool listOfNodes = val.m_kind == Kind::Array && !val.isEmpty()
                && std::any_of(val.m_items.begin(), val.m_items.end(), [](const Node &e) { return e.isContainer(); });
            if (nested || listOfNodes) {
                out += pad + key + ":\n";
                val.writeYaml(indent + 2, out);
            } else if (val.m_kind == Kind::String && val.m_text.find('\n') != std::string::npos) {
                out += pad + key + ": |\n";
                for (const std::string &line : splitLines(val.m_text)) {
                    out += pad + "  " + line + "\n";
                }
            } else {
                out += pad + key + ": " + val.scalar() + "\n";
            }
        }
    } else if (m_kind == Kind::Array) {
        for (const Node &e : m_items) {
            std::string inner;
            e.writeYaml(indent + 2, inner);
            if (e.m_kind == Kind::Object && !inner.empty()) {
                // "- " takes the place of the first line's indent.
                out += pad + "- " + inner.substr(indent + 2);
            } else {
                out += pad + "- " + e.scalar() + "\n";
            }
        }
    } else {
        out += pad + scalar() + "\n";
    }
}

} // namespace rfx
