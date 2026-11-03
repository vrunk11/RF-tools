/******************************************************************************
 * posfile.h
 * rfx library - file access by position, from several threads
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QString>

#include <cstdint>
#include <cstdio>
#include <mutex>

namespace rfx {

// A file read and written at byte offsets, safe to use from many threads at
// once: the samples of a capture are read and written by index, so no caller
// has a cursor of its own. It is the C library's FILE, unbuffered, so that a read
// sees every earlier write and a big read goes straight to the system.
class PosFile
{
public:
    enum class Mode { Read, Write };

    // Mode::Write creates or truncates the file and allows reading it back.
    PosFile(const QString &path, Mode mode);
    ~PosFile();
    PosFile(const PosFile &) = delete;
    PosFile &operator=(const PosFile &) = delete;

    uint64_t size() const;
    // Make the file at least `size` bytes (a new file is zero-filled to it).
    void setSize(uint64_t size);

    // Fill `data` from `offset`, or throw at the end of the file.
    void readAt(void *data, size_t length, uint64_t offset);
    void writeAt(const void *data, size_t length, uint64_t offset);

    const QString &path() const { return m_path; }

private:
    QString m_path;
    mutable std::mutex m_mutex;
    std::FILE *m_file = nullptr;
};

} // namespace rfx
