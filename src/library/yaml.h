/******************************************************************************
 * yaml.h
 * rfx library - a small document tree written as block-style YAML
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rfx {

// A JSON-shaped value, for the metadata documents the tools write. Object keys
// are kept in byte order.
class Node
{
public:
    enum class Kind { Null, Bool, Integer, Float, String, Array, Object };

    Node() = default;
    static Node boolean(bool v);
    static Node integer(int64_t v);
    static Node unsignedInteger(uint64_t v);
    static Node real(double v);
    static Node string(const std::string &v);
    static Node array();
    static Node object();

    Kind kind() const { return m_kind; }
    bool isContainer() const { return m_kind == Kind::Array || m_kind == Kind::Object; }
    bool isEmpty() const { return m_items.empty(); }

    Node &push(const Node &value);
    Node &set(const std::string &key, const Node &value);

    // The document as block-style YAML at `indent` (strings in JSON quoting,
    // which YAML reads; multi-line text as a literal block).
    void writeYaml(size_t indent, std::string &out) const;

private:
    std::string scalar() const;

    Kind m_kind = Kind::Null;
    bool m_bool = false;
    bool m_negative = false;
    uint64_t m_magnitude = 0;
    double m_real = 0;
    std::string m_text;
    std::vector<Node> m_items;
    std::vector<std::string> m_keys;
};

// A real number as JSON writes it: the shortest digits that read back the same
// (as a double, or as a float), laid out as the ryu library does.
std::string formatReal(double v);
std::string formatReal(float v);

// `s` as a JSON string literal.
std::string jsonQuote(const std::string &s);

} // namespace rfx
