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

#include <QFile>
#include <QString>

#include <cstdint>
#include <memory>
#include <mutex>

namespace rfx {

// A file read and written at byte offsets, safe to use from many threads at
// once: the samples of a capture are read and written by index, so no caller
// has a cursor of its own. Unbuffered, so a read sees every earlier write.
class PosFile
{
public:
    enum class Mode { Read, Write };

    // Mode::Write creates or truncates the file and allows reading it back.
    PosFile(const QString &path, Mode mode);

    uint64_t size() const;
    void setSize(uint64_t size);

    // Fill `data` from `offset`, or throw at the end of the file.
    void readAt(void *data, size_t length, uint64_t offset);
    void writeAt(const void *data, size_t length, uint64_t offset);

    const QString &path() const { return m_path; }

private:
    QString m_path;
    mutable std::mutex m_mutex;
    mutable QFile m_file;
};

} // namespace rfx
