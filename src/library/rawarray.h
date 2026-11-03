/******************************************************************************
 * rawarray.h
 * rfx library - an array of numbers whose memory is not filled when it is allocated
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <memory>

namespace rfx {

// A std::vector fills the hundreds of megabytes of a chunk with zeros as it is
// made, in one thread, before anything uses them. This does not: the threads that
// write each part of it touch its pages, in parallel, and write every value they
// own (the others are never read).
template<typename T>
class RawArray
{
public:
    RawArray() = default;
    explicit RawArray(size_t size) : m_data(size > 0 ? new T[size] : nullptr), m_size(size) {}

    T *data() { return m_data.get(); }
    const T *data() const { return m_data.get(); }
    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    T &operator[](size_t i) { return m_data[i]; }
    const T &operator[](size_t i) const { return m_data[i]; }

private:
    std::unique_ptr<T[]> m_data;
    size_t m_size = 0;
};

} // namespace rfx
